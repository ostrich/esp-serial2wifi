#ifndef NETCLIENT_H
#define NETCLIENT_H
#include <esp8266.h>
struct espconn *netConnectionNew(unsigned type, unsigned port, void *owner);
/* Call only after asynchronous DNS/send/connection ownership has ended. */
void netConnectionFree(struct espconn *conn);
char *netString(const char *text, unsigned length);
#endif
