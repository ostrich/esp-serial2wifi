#ifndef SERIAL2WIFI_SERIAL_API_H
#define SERIAL2WIFI_SERIAL_API_H
#include "httpd.h"
bool serialInputPending(void);
int apiSerial(HttpdConnData *);
int apiSerialCapture(HttpdConnData *);
int apiSerialLive(HttpdConnData *);
int apiSerialUART(HttpdConnData *);
int apiSerialSend(HttpdConnData *);
int apiSerialClear(HttpdConnData *);
int apiSerialReset(HttpdConnData *);
#endif
