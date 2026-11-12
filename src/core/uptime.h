#ifndef SERIAL2WIFI_UPTIME_H
#define SERIAL2WIFI_UPTIME_H
#include <stdint.h>

/* Sample at least once per 32-bit microsecond wrap. The existing LED timer
 * samples every three seconds or less, including when its GPIO is disabled. */
typedef struct {
  uint32_t last, seconds, micros;
} UptimeClock;

static inline void uptimeUpdate(UptimeClock *clock, uint32_t now) {
  uint32_t delta = now - clock->last;
  clock->last = now;
  clock->seconds += delta / 1000000;
  clock->micros += delta % 1000000;
  if (clock->micros >= 1000000) {
    clock->seconds++;
    clock->micros -= 1000000;
  }
}
#endif
