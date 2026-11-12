/*
 * File : uart.c
 * This file is part of Espressif's AT+ command set program.
 * Copyright (C) 2013 - 2016, Espressif Systems
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------
 * Heavily modified and enhanced by Thorsten von Eicken in 2015
 */
#include "esp8266.h"
#include "task.h"
#include "uart.h"

#ifdef UART_DBG
#define DBG_UART(format, ...) os_printf(format, ##__VA_ARGS__)
#else
#define DBG_UART(format, ...)                                                                      \
  do {                                                                                             \
  } while (0)
#endif

LOCAL uint8_t uart_recvTaskNum;

// The ISR must drain hardware even while a TCP receive callback blocks in TX.
// Power-of-two indices avoid division (and flash-resident helpers) in the ISR.
#define UART_RX_RING_SIZE 2048U
#define UART_RX_TASK_BUDGET 256U
static volatile uint8_t rx_ring[UART_RX_RING_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile bool rx_task_pending;
static volatile UartRxStats rx_stats;
static ETSTimer rx_retry_timer;

// Called only from the UART ISR or with UART interrupts disabled. If the SDK
// queue is full, leave RX enabled: the periodic retry will schedule delivery.
static void uart_schedule_rx(void) {
  if (rx_head != rx_tail && !rx_task_pending) {
    if (post_usr_task(uart_recvTaskNum, 0))
      rx_task_pending = true;
    else
      rx_stats.task_post_failures++;
  }
}

static void ICACHE_FLASH_ATTR uart_retry_rx(void *arg) {
  ETS_UART_INTR_DISABLE();
  uart_schedule_rx();
  ETS_UART_INTR_ENABLE();
}

// UartDev is defined and initialized in rom code.
extern UartDevice UartDev;
#define MAX_CB 4
static UartRecv_cb uart_recv_cb[4];

static void uart0_rx_intr_handler(void *para);

/******************************************************************************
 * FunctionName : uart_config
 * Description  : Internal used function
 *                UART0 used for data TX/RX, RX buffer size is 0x100, interrupt enabled
 *                UART1 just used for debug output
 * Parameters   : uart_no, use UART0 or UART1 defined ahead
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR uart_config(uint8 uart_no, UartBautRate baudrate, uint32 conf0) {
  if (uart_no == UART1) {
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);
    PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO2_U);
  } else {
    /* rcv_buff size is 0x100 */
    ETS_UART_INTR_ATTACH(uart0_rx_intr_handler, &(UartDev.rcv_buff));
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, 0); // FUNC_U0RXD==0
    //PIN_PULLUP_DIS (PERIPHS_IO_MUX_U0TXD_U); now done in serbridgeInitPins
    //PIN_PULLUP_DIS (PERIPHS_IO_MUX_U0RXD_U);
  }

  uart_div_modify(uart_no, UART_CLK_FREQ / baudrate);

  if (uart_no == UART1) //UART 1 always 8 N 1
    conf0 = CALC_UARTMODE(EIGHT_BITS, NONE_BITS, ONE_STOP_BIT);
  WRITE_PERI_REG(UART_CONF0(uart_no), conf0);

  //clear rx and tx fifo,not ready
  SET_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);
  CLEAR_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);

  if (uart_no == UART0) {
    // Configure RX interrupt conditions as follows: trigger rx-full when there are 80 characters
    // in the buffer, trigger rx-timeout when the fifo is non-empty and nothing further has been
    // received for 4 character periods.
    // Set the hardware flow-control to trigger when the FIFO holds 100 characters, although
    // we don't really expect the signals to actually be wired up to anything. It doesn't hurt
    // to set the threshold here...
    // We do not enable framing error interrupts 'cause they tend to cause an interrupt avalanche
    // and instead just poll for them when we get a std RX interrupt.
    WRITE_PERI_REG(UART_CONF1(uart_no),
                   ((80 & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) |
                       ((100 & UART_RX_FLOW_THRHD) << UART_RX_FLOW_THRHD_S) | UART_RX_FLOW_EN |
                       (4 & UART_RX_TOUT_THRHD) << UART_RX_TOUT_THRHD_S | UART_RX_TOUT_EN);
    SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA |
                                                 UART_RXFIFO_OVF_INT_ENA);
  } else {
    WRITE_PERI_REG(UART_CONF1(uart_no), ((UartDev.rcv_buff.TrigLvl & UART_RXFIFO_FULL_THRHD)
                                         << UART_RXFIFO_FULL_THRHD_S));
  }

  //clear all interrupt
  WRITE_PERI_REG(UART_INT_CLR(uart_no), 0xffff);
}

/******************************************************************************
 * FunctionName : uart1_tx_one_char
 * Description  : Internal used function
 *                Use uart1 interface to transfer one char
 * Parameters   : uint8 TxChar - character to tx
 * Returns      : OK
*******************************************************************************/
STATUS
uart_tx_one_char(uint8 uart, uint8 c) {
  //Wait until there is room in the FIFO
  while (((READ_PERI_REG(UART_STATUS(uart)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT) >= 100)
    ;
  //Send the character
  WRITE_PERI_REG(UART_FIFO(uart), c);
  return OK;
}

/******************************************************************************
 * FunctionName : uart1_write_char
 * Description  : Internal used function
 *                Do some special deal while tx char is '\r' or '\n'
 * Parameters   : char c - character to tx
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR uart1_write_char(char c) {
  //if (c == '\n') uart_tx_one_char(UART1, '\r');
  uart_tx_one_char(UART1, c);
}
void ICACHE_FLASH_ATTR uart0_write_char(char c) {
  //if (c == '\n') uart_tx_one_char(UART0, '\r');
  uart_tx_one_char(UART0, c);
}
/******************************************************************************
 * FunctionName : uart0_tx_buffer
 * Description  : use uart0 to transfer buffer
 * Parameters   : uint8 *buf - point to send buffer
 *                uint16 len - buffer len
 * Returns      :
*******************************************************************************/
unsigned ICACHE_FLASH_ATTR uart0_tx_available(void) {
  unsigned count = (READ_PERI_REG(UART_STATUS(UART0)) >> UART_TXFIFO_CNT_S) & UART_TXFIFO_CNT;
  return count < 100 ? 100 - count : 0;
}
void ICACHE_FLASH_ATTR uart0_tx_buffer(char *buf, uint16 len) {
  uint16 i;

  for (i = 0; i < len; i++) {
    uart_tx_one_char(UART0, buf[i]);
  }
}

/******************************************************************************
 * FunctionName : uart0_sendStr
 * Description  : use uart0 to transfer buffer
 * Parameters   : uint8 *buf - point to send buffer
 *                uint16 len - buffer len
 * Returns      :
*******************************************************************************/
void ICACHE_FLASH_ATTR uart0_sendStr(const char *str) {
  while (*str) {
    uart_tx_one_char(UART0, *str++);
  }
}

// All code executed by the ISR stays in IRAM. Do not log from this path:
// logging can call flash code and delay draining the hardware FIFO.
static void uart0_rx_intr_handler(void *para) {
  uint32_t raw = READ_PERI_REG(UART_INT_RAW(UART0));
  if (raw & UART_RXFIFO_OVF_INT_RAW)
    rx_stats.fifo_overruns++;
  if (raw & UART_FRM_ERR_INT_RAW) {
    rx_stats.framing_errors++;
    rx_stats.framing_drops +=
        (READ_PERI_REG(UART_STATUS(UART0)) >> UART_RXFIFO_CNT_S) & UART_RXFIFO_CNT;
    // Keep the SDK driver's framing-error recovery, but account for loss.
    SET_PERI_REG_MASK(UART_CONF0(UART0), UART_RXFIFO_RST);
    CLEAR_PERI_REG_MASK(UART_CONF0(UART0), UART_RXFIFO_RST);
  }

  // Bound interrupt work to the FIFO occupancy seen on entry. New arrivals
  // can trigger another interrupt instead of trapping us in a receive loop.
  uint32_t count = (READ_PERI_REG(UART_STATUS(UART0)) >> UART_RXFIFO_CNT_S) & UART_RXFIFO_CNT;
  while (count--) {
    uint8_t byte = READ_PERI_REG(UART_FIFO(UART0)) & 0xff;
    uint32_t head = rx_head;
    rx_stats.bytes_received++;
    if (head - rx_tail < UART_RX_RING_SIZE) {
      rx_ring[head & (UART_RX_RING_SIZE - 1)] = byte;
      rx_head = head + 1;
      uint32_t pending = rx_head - rx_tail;
      if (pending > rx_stats.high_water)
        rx_stats.high_water = pending;
    } else {
      rx_stats.ring_drops++;
    }
  }
  WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR |
                                          UART_RXFIFO_OVF_INT_CLR | UART_FRM_ERR_INT_CLR);
  uart_schedule_rx();
}

static void ICACHE_FLASH_ATTR uart_recvTask(os_event_t *events) {
  char buf[UART_RX_TASK_BUDGET];
  uint32_t tail = rx_tail;
  uint32_t available = rx_head - tail;
  uint16_t length = available > sizeof(buf) ? sizeof(buf) : available;
  for (uint16_t i = 0; i < length; i++)
    buf[i] = rx_ring[(tail + i) & (UART_RX_RING_SIZE - 1)];
  rx_tail = tail + length;
  rx_stats.bytes_delivered += length;

  if (length) {
    for (int i = 0; i < MAX_CB; i++) {
      if (uart_recv_cb[i] != NULL)
        uart_recv_cb[i](buf, length);
    }
  }

  // Yield to the SDK after each bounded batch. The ISR remains enabled while
  // callbacks run, and continues filling the software ring independently.
  ETS_UART_INTR_DISABLE();
  rx_task_pending = false;
  uart_schedule_rx();
  ETS_UART_INTR_ENABLE();
}

void ICACHE_FLASH_ATTR uart_get_rx_stats(UartRxStats *stats) {
  ETS_UART_INTR_DISABLE();
  *stats = rx_stats;
  stats->pending = rx_head - rx_tail;
  ETS_UART_INTR_ENABLE();
}

// Turn UART interrupts off and poll for nchars or until timeout hits
uint16_t ICACHE_FLASH_ATTR uart0_rx_poll(char *buff, uint16_t nchars, uint32_t timeout_us) {
  ETS_UART_INTR_DISABLE();
  uint16_t got = 0;
  if (nchars == 0)
    goto done;
  // Programming paths may poll after the ISR has already buffered a reply.
  while (rx_tail != rx_head && got < nchars) {
    buff[got++] = rx_ring[rx_tail & (UART_RX_RING_SIZE - 1)];
    rx_tail++;
    rx_stats.bytes_polled++;
  }
  if (got == nchars)
    goto done;
  uint32_t start = system_get_time(); // time in us
  while (system_get_time() - start < timeout_us) {
    while (READ_PERI_REG(UART_STATUS(UART0)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S)) {
      buff[got++] = READ_PERI_REG(UART_FIFO(UART0)) & 0xFF;
      rx_stats.bytes_received++;
      rx_stats.bytes_polled++;
      if (got == nchars)
        goto done;
    }
  }
done:
  ETS_UART_INTR_ENABLE();
  return got;
}

void ICACHE_FLASH_ATTR uart0_baud(int rate) {
  os_printf("UART %d baud\n", rate);
  uart_div_modify(UART0, UART_CLK_FREQ / rate);
}

void ICACHE_FLASH_ATTR uart0_config(uint8_t data_bits, uint8_t parity, uint8_t stop_bits) {
  uint32_t conf0 = CALC_UARTMODE(data_bits, parity, stop_bits);
  WRITE_PERI_REG(UART_CONF0(0), conf0);
}

/******************************************************************************
 * FunctionName : uart_init
 * Description  : user interface for init uart
 * Parameters   : UartBautRate uart0_br - uart0 bautrate
 *                UartBautRate uart1_br - uart1 bautrate
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR uart_init(uint32 conf0, UartBautRate uart0_br, UartBautRate uart1_br) {
  // Register before enabling RX: early input must never post an invalid task.
  uart_recvTaskNum = register_usr_task(uart_recvTask);
  os_timer_setfn(&rx_retry_timer, uart_retry_rx, NULL);
  os_timer_arm(&rx_retry_timer, 10, 1);
  // rom use 74880 baut_rate, here reinitialize
  uart_config(UART0, uart0_br, conf0);
  uart_config(UART1, uart1_br, conf0);
  for (int i = 0; i < 4; i++)
    uart_tx_one_char(UART1, '\n');
  for (int i = 0; i < 4; i++)
    uart_tx_one_char(UART0, '\n');
  ETS_UART_INTR_ENABLE();

  // install uart1 putc callback
  os_install_putc1((void *)uart0_write_char);
}

void ICACHE_FLASH_ATTR uart_add_recv_cb(UartRecv_cb cb) {
  for (int i = 0; i < MAX_CB; i++) {
    if (uart_recv_cb[i] == NULL) {
      uart_recv_cb[i] = cb;
      return;
    }
  }
  os_printf("UART: max cb count exceeded\n");
}
