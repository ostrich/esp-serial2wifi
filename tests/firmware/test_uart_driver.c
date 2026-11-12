// Execute the actual driver with a simulated FIFO/task queue, not a copy of it.
#include "esp8266.h"
#ifndef UART_DRIVER_SOURCE
#define UART_DRIVER_SOURCE "../src/serial/uart.c"
#endif
#include UART_DRIVER_SOURCE
UartDevice UartDev;
static uint8 fifo[16384], received[16384];
static unsigned fifo_rd, fifo_wr, received_len, queued, refill;
static uint32 raw_flags, conf0;
static bool fail_post;

static uint32 test_read(uint32 reg) {
  if (reg == UART_STATUS(UART0))
    return (fifo_wr - fifo_rd) << UART_RXFIFO_CNT_S;
  if (reg == UART_INT_RAW(UART0))
    return raw_flags;
  if (reg == UART_CONF0(UART0))
    return conf0;
  if (reg == UART_FIFO(UART0)) {
    assert(fifo_rd < fifo_wr);
    uint8 byte = fifo[fifo_rd++];
    if (refill) {
      fifo[fifo_wr++] = 0xa5;
      refill--;
    }
    return byte;
  }
  return 0;
}
static void test_write(uint32 reg, uint32 value) {
  if (reg == UART_INT_CLR(UART0))
    raw_flags &= ~value;
  if (reg == UART_CONF0(UART0)) {
    conf0 = value;
    if (value & UART_RXFIFO_RST)
      fifo_rd = fifo_wr;
  }
}
bool post_usr_task(uint8_t task, os_param_t par) {
  (void)task;
  (void)par;
  if (fail_post)
    return false;
  queued++;
  assert(queued == 1); // A single pending task coalesces incoming interrupts.
  return true;
}
uint8_t register_usr_task(os_task_t task) {
  (void)task;
  return 3;
}
static void collect(char *buf, short len) {
  assert(!in_isr && irq_enabled);
  memcpy(received + received_len, buf, len);
  received_len += len;
}
static void reset(void) {
  memset((void *)&rx_stats, 0, sizeof(rx_stats));
  memset(uart_recv_cb, 0, sizeof(uart_recv_cb));
  rx_head = rx_tail = 0;
  rx_task_pending = false;
  fifo_rd = fifo_wr = received_len = queued = refill = 0;
  raw_flags = conf0 = 0;
  fail_post = false;
  irq_enabled = true;
  in_isr = false;
  uart_add_recv_cb(collect);
}
static void run_task(void) {
  assert(queued);
  queued--;
  uart_recvTask(NULL);
}
static void interrupt(void) {
  assert(irq_enabled);
  in_isr = true;
  uart0_rx_intr_handler(NULL);
  in_isr = false;
  assert(irq_enabled);
}
static void input(unsigned count, unsigned seed) {
  assert(fifo_wr - fifo_rd + count <= 128);
  for (unsigned i = 0; i < count; i++)
    fifo[fifo_wr++] = (uint8)(seed + i);
  raw_flags |= UART_RXFIFO_FULL_INT_RAW;
  interrupt();
}
int main(void) {
  reset();
  input(128, 0);
  input(128, 128);
  input(128, 0);
  input(128, 128);
  assert(queued == 1 && rx_head - rx_tail == 512);
  run_task();
  assert(received_len == 256 && queued == 1 && rx_head - rx_tail == 256);
  run_task();
  assert(received_len == 512 && queued == 0);
  for (unsigned i = 0; i < 512; i++)
    assert(received[i] == (uint8)i);

  // Full ring: drain hardware, count every dropped byte, and preserve order.
  reset();
  for (unsigned i = 0; i < 40; i++)
    input(128, i * 128);
  assert(rx_stats.bytes_received == 5120 && rx_stats.ring_drops == 5120 - UART_RX_RING_SIZE);
  assert(rx_stats.high_water == UART_RX_RING_SIZE && fifo_rd == fifo_wr);
  while (queued)
    run_task();
  assert(received_len == UART_RX_RING_SIZE);
  for (unsigned i = 0; i < UART_RX_RING_SIZE; i++)
    assert(received[i] == (uint8)i);
  input(128, 7);
  run_task();
  for (unsigned i = 0; i < 128; i++)
    assert(received[UART_RX_RING_SIZE + i] == (uint8)(7 + i));

  // Failed task posts neither disable RX nor strand queued bytes.
  reset();
  fail_post = true;
  input(64, 11);
  assert(!queued && rx_stats.task_post_failures == 1 && irq_enabled);
  fail_post = false;
  uart_retry_rx(NULL);
  assert(queued == 1);
  run_task();
  assert(received_len == 64);

  // Unsigned producer/consumer counters and indices wrap correctly.
  reset();
  rx_head = rx_tail = UINT32_MAX - 63;
  input(128, 9);
  run_task();
  assert(received_len == 128 && rx_head == 64 && rx_tail == 64);
  for (unsigned i = 0; i < 128; i++)
    assert(received[i] == (uint8)(i + 9));

  // Polling programming paths must see bytes buffered before polling began.
  reset();
  input(32, 40);
  char polled[64];
  assert(uart0_rx_poll(polled, 16, 100) == 16);
  assert(rx_stats.bytes_polled == 16);
  run_task();
  assert(received_len == 16 && (uint8)received[0] == 56);
  assert(uart0_rx_poll(polled, 0, 100) == 0);

  // ISR processing ends after its entry snapshot, despite new arrivals.
  reset();
  refill = 64;
  input(128, 0);
  assert(rx_stats.bytes_received == 128 && fifo_wr - fifo_rd == 64);
  interrupt();
  while (queued)
    run_task();
  assert(received_len == 192);

  reset();
  raw_flags = UART_FRM_ERR_INT_RAW | UART_RXFIFO_OVF_INT_RAW;
  for (unsigned i = 0; i < 10; i++)
    fifo[fifo_wr++] = 0;
  interrupt();
  assert(rx_stats.framing_errors == 1 && rx_stats.framing_drops == 10);
  assert(rx_stats.fifo_overruns == 1 && fifo_rd == fifo_wr && !queued);
  puts(
      "PASS actual UART driver: bounded ISR/task, ordering, ring overflow/wrap, post retry, polling, framing recovery");
  return 0;
}
