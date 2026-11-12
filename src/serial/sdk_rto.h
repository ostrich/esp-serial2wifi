#ifndef ESP_SDK_RTO_H
#define ESP_SDK_RTO_H
#include <stdint.h>
struct espconn;
void sdk_rto_attach(struct espconn *);
void sdk_rto_detach(struct espconn *);
void sdk_rto_prepare_send(struct espconn *);
void sdk_rto_sent(struct espconn *);
void sdk_rto_acked(struct espconn *);
void sdk_rto_json(char buf[1024]);
int sdk_tx_sequence(struct espconn *, uint32_t *);
int sdk_tx_copy_complete(struct espconn *, const void *, uint16_t, uint32_t);
#endif
