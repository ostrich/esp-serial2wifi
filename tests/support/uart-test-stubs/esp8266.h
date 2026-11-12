#ifndef ESP8266_TEST_STUBS_H
#define ESP8266_TEST_STUBS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int STATUS;
typedef uintptr_t os_param_t;
typedef struct {
  unsigned sig;
  os_param_t par;
} os_event_t;
typedef void (*os_task_t)(os_event_t *);
typedef struct {
  void (*fn)(void *);
  void *arg;
} ETSTimer;
#define LOCAL static
#define ICACHE_FLASH_ATTR
#define OK 0
#define UART_CLK_FREQ 80000000U
#define BIT(n) (1U << (n))
#define BIT2 BIT(2)
#define BIT3 BIT(3)
#define BIT4 BIT(4)
#define BIT5 BIT(5)
#include "uart_hw.h"
static uint32 test_read(uint32 reg);
static void test_write(uint32 reg, uint32 value);
static bool irq_enabled, in_isr;
#define READ_PERI_REG(reg) test_read(reg)
#define WRITE_PERI_REG(reg, val) test_write(reg, val)
#define SET_PERI_REG_MASK(reg, val) test_write(reg, test_read(reg) | (val))
#define CLEAR_PERI_REG_MASK(reg, val) test_write(reg, test_read(reg) & ~(val))
#define ETS_UART_INTR_DISABLE() (irq_enabled = false)
#define ETS_UART_INTR_ENABLE() (irq_enabled = true)
#define ETS_UART_INTR_ATTACH(fn, arg) ((void)0)
#define PIN_FUNC_SELECT(pin, func) ((void)0)
#define PIN_PULLUP_DIS(pin) ((void)0)
#define os_printf(...)                                                                             \
  do {                                                                                             \
    assert(!in_isr);                                                                               \
  } while (0)
static void uart_div_modify(uint8 uart, uint32 divisor) {
  (void)uart;
  (void)divisor;
}
static void os_timer_setfn(ETSTimer *timer, void (*fn)(void *), void *arg) {
  timer->fn = fn;
  timer->arg = arg;
}
static void os_timer_arm(ETSTimer *timer, uint32 ms, bool repeat) {
  (void)timer;
  (void)ms;
  (void)repeat;
}
static void os_install_putc1(void *fn) { (void)fn; }
static uint32 system_get_time(void) {
  static uint32 now;
  return ++now;
}
#endif
