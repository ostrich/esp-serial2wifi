#ifndef SERIAL2WIFI_NETWORK_H
#define SERIAL2WIFI_NETWORK_H
#include "httpd.h"
int apiNetwork(HttpdConnData *);
int apiNetworkStation(HttpdConnData *);
int apiNetworkAP(HttpdConnData *);
int apiNetworkIdentity(HttpdConnData *);
int apiNetworkScan(HttpdConnData *);
int apiNetworkScanPage(HttpdConnData *);
#endif
