#ifndef SERIAL2WIFI_MANAGEMENT_H
#define SERIAL2WIFI_MANAGEMENT_H
#include "httpd.h"
int apiSettings(HttpdConnData *);
int apiIntegrations(HttpdConnData *);
int apiManagementSave(HttpdConnData *);
int apiManagementRestart(HttpdConnData *);
#endif
