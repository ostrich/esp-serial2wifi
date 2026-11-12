/* Bridge send credit and microsecond RTO policy; pinned SDK code stays intact. */
#ifndef RTO_HOST_TEST
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcomment"
#include "lwip/tcp_impl.h"
#include "lwip/app/espconn.h"
#pragma GCC diagnostic pop
/* This SDK header declares a private static ringbuf_nextp without a body.
 * GCC diagnoses that declaration at end of translation unit. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "sdk_rto.h"
#define ABI(n, e) typedef char abi_##n[(e) ? 1 : -1]
ABI(pcb_size, sizeof(struct tcp_pcb) == 176);
#define OFF(f, n) ABI(f, __builtin_offsetof(struct tcp_pcb, f) == n)
OFF(next, 12);
OFF(state, 16);
OFF(flags, 36);
OFF(rtime, 58);
OFF(rto, 76);
OFF(mss, 60);
OFF(snd_buf, 114);
OFF(snd_queuelen, 116);
OFF(snd_lbb, 108);
OFF(unsent, 120);
OFF(nrtx, 78);
OFF(lastack, 80);
OFF(snd_wnd, 96);
OFF(unacked, 124);
OFF(local_port, 32);
OFF(remote_port, 34);
OFF(remote_ip, 4);
OFF(callback_arg, 24);
ABI(msg_connection, __builtin_offsetof(espconn_msg, pespconn) == 0);
ABI(msg_pcb, __builtin_offsetof(espconn_msg, pcommon.pcb) == 4);
ABI(msg_buffers, __builtin_offsetof(espconn_msg, pcommon.pbuf) == 24);
ABI(buffer_payload, __builtin_offsetof(espconn_buf, payload) == 0);
ABI(buffer_unsent, __builtin_offsetof(espconn_buf, unsent) == 8);
ABI(buffer_next, __builtin_offsetof(espconn_buf, pnext) == 16);
ABI(pbuf_payload, __builtin_offsetof(struct pbuf, payload) == 4);
ABI(pbuf_length, __builtin_offsetof(struct pbuf, len) == 10);
ABI(segment_pbuf, __builtin_offsetof(struct tcp_seg, p) == 4);
#endif
#include "rto_policy.h"
extern uint32 system_get_time(void);
extern void vendor_tcp_slowtmr(void);
extern void vendor_tcp_rexmit_rto(struct tcp_pcb *);
extern void vendor_tcp_rexmit_fast(struct tcp_pcb *);
struct flight {
  uint32_t when;
  unsigned ambiguous;
};
struct tracker {
  struct espconn *conn;
  struct tcp_pcb *pcb;
  void *pcb_owner; // SDK node identity, so a reused PCB address cannot inherit policy.
  struct rto_estimator estimate;
  struct flight flights[4];
  uint32_t anchor, lastack;
  unsigned head, count, configured;
};
static struct tracker tracked[4];
static unsigned window_grants, window_declined;
static unsigned samples, ambiguous, timeouts, fast_retries, due, guarded;
static unsigned copy_releases;
static uint32_t last_base, last_target, last_elapsed, min_fire_elapsed = UINT32_MAX;
static struct tracker *ICACHE_FLASH_ATTR by_conn(struct espconn *conn) {
  unsigned i;
  for (i = 0; i < 4; i++)
    if (tracked[i].conn == conn)
      return &tracked[i];
  return NULL;
}
static struct tracker *ICACHE_FLASH_ATTR by_pcb(struct tcp_pcb *pcb) {
  unsigned i;
  for (i = 0; i < 4; i++)
    if (tracked[i].conn && tracked[i].pcb == pcb && pcb->local_port == 23 &&
        tracked[i].pcb_owner == pcb->callback_arg)
      return &tracked[i];
  return NULL;
}
static struct tcp_pcb *ICACHE_FLASH_ATTR find_pcb(struct espconn *conn) {
  struct tcp_pcb *p = tcp_active_pcbs;
  unsigned n = 0;
  while (p && n++ < 16) {
    if (p->local_port == conn->proto.tcp->local_port &&
        p->remote_port == conn->proto.tcp->remote_port &&
        !os_memcmp(&p->remote_ip, conn->proto.tcp->remote_ip, 4))
      return p;
    p = p->next;
  }
  return NULL;
}
int ICACHE_FLASH_ATTR sdk_tx_sequence(struct espconn *conn, uint32_t *sequence) {
  struct tcp_pcb *p = find_pcb(conn);
  espconn_msg *message;
  if (!p || !p->callback_arg)
    return 0;
  message = p->callback_arg;
  if (message->pespconn != conn || message->pcommon.pcb != p)
    return 0;
  *sequence = p->snd_lbb;
  return 1;
}
static int ICACHE_FLASH_ATTR overlaps(const void *a, unsigned length, const void *b,
                                      unsigned size) {
  uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
  if (!length || !size)
    return 0;
  return x >= y ? x - y < size : y - x < length;
}
int ICACHE_FLASH_ATTR sdk_tx_copy_complete(struct espconn *conn, const void *data, uint16_t length,
                                           uint32_t end_sequence) {
  struct tcp_pcb *p = find_pcb(conn);
  espconn_msg *message;
  espconn_buf *record;
  struct tcp_seg *segment;
  unsigned n = 0, q;
  if (!p || !p->callback_arg || (int32_t)(p->snd_lbb - end_sequence) < 0)
    return 0;
  message = p->callback_arg;
  if (message->pespconn != conn || message->pcommon.pcb != p)
    return 0;
  for (record = message->pcommon.pbuf; record; record = record->pnext) {
    if (++n > 8)
      return 0;
    if (record->unsent && overlaps(record->payload, record->len, data, length))
      return 0;
  }
  /* Verify actual TCP ownership, rather than relying on the COPY option's
   * confusing name or on completion notifications that carry no record ID. */
  for (q = 0; q < 2; q++) {
    n = 0;
    for (segment = q ? p->unacked : p->unsent; segment; segment = segment->next) {
      struct pbuf *buffer;
      unsigned count = 0;
      if (++n > 16 || !segment->p)
        return 0;
      for (buffer = segment->p; buffer; buffer = buffer->next) {
        if (++count > 16 || overlaps(buffer->payload, buffer->len, data, length))
          return 0;
      }
    }
  }
  copy_releases++;
  return 1;
}
void ICACHE_FLASH_ATTR sdk_rto_attach(struct espconn *conn) {
  struct tcp_pcb *p;
  unsigned i;
  if (conn->proto.tcp->local_port != 23)
    return;
  p = find_pcb(conn);
  if (!p || !p->callback_arg)
    return;
  for (i = 0; i < 4; i++)
    if (!tracked[i].conn) {
      os_memset(&tracked[i], 0, sizeof(tracked[i]));
      tracked[i].conn = conn;
      tracked[i].pcb = p;
      tracked[i].pcb_owner = p->callback_arg;
      tracked[i].anchor = system_get_time();
      tracked[i].lastack = p->lastack;
      return;
    }
}
void ICACHE_FLASH_ATTR sdk_rto_detach(struct espconn *conn) {
  struct tracker *t = by_conn(conn);
  if (t)
    os_memset(t, 0, sizeof(*t));
}
void ICACHE_FLASH_ATTR sdk_rto_prepare_send(struct espconn *conn) {
  struct tracker *t = by_conn(conn);
  struct tcp_pcb *p;
  if (!t || t->configured || t->count)
    return;
  p = find_pcb(conn);
  if (!p || p != t->pcb || p->callback_arg != t->pcb_owner)
    return;
  /* Configure initial byte credit only, before any application data or pbuf.
   * SDK queue limit remains8; four one-pbuf536-byte sends fit that limit.
   * ACKs add back admitted bytes; no compile-time1072 cap exists there. */
  if (p->state == ESTABLISHED && p->mss == 536 && p->snd_buf == 1072 && !p->snd_queuelen &&
      !p->unsent && !p->unacked && p->snd_lbb == p->lastack) {
    p->snd_buf = 2144;
    window_grants++;
    t->configured = 1;
  } else
    window_declined++;
}
void ICACHE_FLASH_ATTR sdk_rto_sent(struct espconn *conn) {
  struct tracker *t = by_conn(conn);
  unsigned n;
  if (!t || find_pcb(conn) != t->pcb || t->pcb->callback_arg != t->pcb_owner)
    return;
  if (t->count == 4) {
    sdk_rto_detach(conn);
    return;
  } /* Disable on accounting mismatch. */
  if (!t->count) {
    t->anchor = system_get_time();
    t->lastack = t->pcb->lastack;
  }
  n = (t->head + t->count) % 4;
  t->flights[n].when = system_get_time();
  t->flights[n].ambiguous = 0;
  t->count++;
}
void ICACHE_FLASH_ATTR sdk_rto_acked(struct espconn *conn) {
  struct tracker *t = by_conn(conn);
  uint32_t now = system_get_time(), r;
  if (!t || !t->count || find_pcb(conn) != t->pcb || t->pcb->callback_arg != t->pcb_owner)
    return;
  r = now - t->flights[t->head].when;
  if (!t->flights[t->head].ambiguous && r <= RTO_MAX_US) {
    rto_sample(&t->estimate, r);
    samples++;
    last_base = rto_base(&t->estimate);
  } else
    ambiguous++;
  t->head = (t->head + 1) % 4;
  t->count--;
  t->anchor = now;
  t->lastack = t->pcb->lastack;
}
static void ICACHE_FLASH_ATTR mark_retry(struct tracker *t) {
  unsigned i;
  if (!t)
    return;
  for (i = 0; i < t->count; i++)
    t->flights[(t->head + i) % 4].ambiguous = 1;
  t->anchor = system_get_time();
}
void ICACHE_FLASH_ATTR tcp_rexmit_rto(struct tcp_pcb *p) {
  struct tracker *t = by_pcb(p);
  if (t && p->unacked) {
    uint32_t elapsed = system_get_time() - t->anchor;
    if (elapsed < min_fire_elapsed)
      min_fire_elapsed = elapsed;
    timeouts++;
    mark_retry(t);
  }
  vendor_tcp_rexmit_rto(p);
}
void ICACHE_FLASH_ATTR tcp_rexmit_fast(struct tcp_pcb *p) {
  struct tracker *t = by_pcb(p);
  if (t && p->unacked && !(p->flags & TF_INFR)) {
    fast_retries++;
    mark_retry(t);
  }
  vendor_tcp_rexmit_fast(p);
}
void ICACHE_FLASH_ATTR tcp_slowtmr(void) {
  struct tcp_pcb *p = tcp_active_pcbs;
  unsigned n = 0;
  uint32_t now = system_get_time();
  while (p && n++ < 16) {
    struct tracker *t = by_pcb(p);
    if (t && t->estimate.samples && t->count && p->state == ESTABLISHED && p->unacked &&
        p->rtime >= 0 && p->snd_wnd) {
      /* A partial ACK can advance TCP without completing an espconn record. */
      if (t->lastack != p->lastack) {
        t->lastack = p->lastack;
        t->anchor = now;
      }
      last_target = rto_backed_off(&t->estimate, p->nrtx);
      last_elapsed = now - t->anchor;
      /* Original slowtmr increments rtime then compares it to rto. Keep it
       * from firing until the absolute microsecond deadline has passed.
       * No new timer, queue movement, cwnd change or pbuf ownership operation. */
      if (p->rtime < 32765) {
        if (last_elapsed >= last_target) {
          p->rto = p->rtime + 1;
          due++;
        } else {
          p->rto = p->rtime + 2;
          guarded++;
        }
      }
    }
    p = p->next;
  }
  vendor_tcp_slowtmr();
}
void ICACHE_FLASH_ATTR sdk_rto_json(char buf[1024]) {
  os_sprintf(buf,
             "{\"window_grants\":%u,\"window_declined\":%u,\"samples\":%u,\"ambiguous_samples_"
             "rejected\":%u,\"rto_retries\":%u,"
             "\"fast_retries\":%u,\"due_checks\":%u,\"guarded_checks\":%u,"
             "\"last_base_us\":%u,\"last_target_us\":%u,\"last_elapsed_us\":%u,\"min_fire_"
             "elapsed_us\":%u,\"copy_releases\":%u}",
             window_grants, window_declined, samples, ambiguous, timeouts, fast_retries, due,
             guarded, last_base, last_target, last_elapsed, min_fire_elapsed, copy_releases);
}
