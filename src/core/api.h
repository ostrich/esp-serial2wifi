#ifndef SERIAL2WIFI_API_H
#define SERIAL2WIFI_API_H
#include "httpd.h"
int apiCapabilities(HttpdConnData *conn);
int apiStatus(HttpdConnData *conn);
#endif
