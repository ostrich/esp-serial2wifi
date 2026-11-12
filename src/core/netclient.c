#include "netclient.h"
char *ICACHE_FLASH_ATTR netString(const char *text, unsigned length) {
  char *s = os_malloc(length + 1);
  if (s) {
    os_memcpy(s, text, length);
    s[length] = 0;
  }
  return s;
}
void ICACHE_FLASH_ATTR netConnectionFree(struct espconn *c) {
  if (!c)
    return;
  if (c->proto.tcp)
    os_free(c->proto.tcp);
  os_free(c);
}
struct espconn *ICACHE_FLASH_ATTR netConnectionNew(unsigned type, unsigned port, void *owner) {
  struct espconn *c = os_zalloc(sizeof(*c));
  if (!c)
    return NULL;
  c->type = type;
  c->reverse = owner;
  void *protocol = os_zalloc(type == ESPCONN_UDP ? sizeof(esp_udp) : sizeof(esp_tcp));
  if (!protocol) {
    os_free(c);
    return NULL;
  }
  if (type == ESPCONN_UDP) {
    c->proto.udp = protocol;
    c->proto.udp->local_port = espconn_port();
    c->proto.udp->remote_port = port;
  } else {
    c->proto.tcp = protocol;
    c->proto.tcp->local_port = espconn_port();
    c->proto.tcp->remote_port = port;
  }
  return c;
}
