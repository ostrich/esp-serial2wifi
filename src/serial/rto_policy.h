/* Microsecond RTT estimator. See docs/development.md#code-and-storage-layout for minimum-RTO policy. */
#ifndef ESP_RTO_POLICY_H
#define ESP_RTO_POLICY_H
#include <stdint.h>
#define RTO_MIN_US 200000U
#define RTO_MAX_US 60000000U
struct rto_estimator {
  uint32_t srtt, variation, samples;
};
static uint32_t rto_base(const struct rto_estimator *e) {
  uint32_t v = e->variation > RTO_MAX_US / 4 ? RTO_MAX_US : 4 * e->variation;
  uint32_t n;
  if (!v)
    v = 1;
  n = e->srtt > RTO_MAX_US - v ? RTO_MAX_US : e->srtt + v;
  return n < RTO_MIN_US ? RTO_MIN_US : n;
}
static void rto_sample(struct rto_estimator *e, uint32_t r) {
  uint32_t d;
  if (!r)
    r = 1;
  if (r > RTO_MAX_US)
    return;
  if (!e->samples) {
    e->srtt = r;
    e->variation = r / 2;
  } else {
    d = r > e->srtt ? r - e->srtt : e->srtt - r;
    /* RTTVAR must use the previous SRTT, before the SRTT update. */
    e->variation = (3 * e->variation + d + 3) / 4;
    e->srtt = (7 * e->srtt + r + 7) / 8;
  }
  if (e->samples != UINT32_MAX)
    e->samples++;
}
static uint32_t rto_backed_off(const struct rto_estimator *e, unsigned retries) {
  uint32_t n = rto_base(e);
  if (retries > 7)
    retries = 7; /* SDK's published/shipped backoff ceiling. */
  while (retries--)
    n = n > RTO_MAX_US / 2 ? RTO_MAX_US : n * 2;
  return n;
}
#endif
