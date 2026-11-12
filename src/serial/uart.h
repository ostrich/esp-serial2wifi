#ifndef __UART_H__
#define __UART_H__

#include "uart_hw.h"

typedef struct {
  uint32_t bytes_received, bytes_delivered, bytes_polled;
  uint32_t ring_drops, fifo_overruns, framing_errors, framing_drops;
  uint32_t task_post_failures, high_water, pending;
} UartRxStats;
void uart_get_rx_stats(UartRxStats *stats);

// Receive callback function signature
typedef void (*UartRecv_cb)(char *buf, short len);

// Initialize UARTs to the provided baud rates (115200 recommended). This also makes the os_printf
// calls use uart1 for output (for debugging purposes)
void uart_init(uint32 conf0, UartBautRate uart0_br, UartBautRate uart1_br);

// Transmit a buffer of characters on UART0
void uart0_tx_buffer(char *buf, uint16 len);
unsigned uart0_tx_available(void);

void uart0_write_char(char c);
STATUS uart_tx_one_char(uint8 uart, uint8 c);

void uart1_write_char(char c);

// Add a receive callback function, this is called on the uart receive task each time a chunk
// of bytes are received. A small number of callbacks can be added and they are all called
// with all new characters.
void uart_add_recv_cb(UartRecv_cb cb);

// Turn UART interrupts off and poll for nchars or until timeout hits
uint16_t uart0_rx_poll(char *buff, uint16_t nchars, uint32_t timeout_us);

void uart0_baud(int rate);
void uart0_config(uint8_t data_bits, uint8_t parity, uint8_t stop_bits);
void uart_config(uint8 uart_no, UartBautRate baudrate, uint32 conf0);

#endif /* __UART_H__ */
