#ifndef SERIAL2WIFI_UPDATES_H
#define SERIAL2WIFI_UPDATES_H
#include "httpd.h"
int apiFirmwareInfo(HttpdConnData *);
int apiFirmwareUpload(HttpdConnData *);
int apiFirmwareActivate(HttpdConnData *);
int apiPagesInfo(HttpdConnData *);
int apiPagesUpload(HttpdConnData *);
int apiPagesClear(HttpdConnData *);
#endif
