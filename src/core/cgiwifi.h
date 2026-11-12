#ifndef CGIWIFI_H
#define CGIWIFI_H

#include "httpd.h"

enum { wifiIsDisconnected, wifiIsConnected, wifiGotIP };
typedef void(*WifiStateChangeCb)(uint8_t wifiStatus);

void configWifiIP();
void wifiInit(void);
void wifiNetworkPause(void);
void wifiNetworkResume(void);
void wifiAddStateChangeCb(WifiStateChangeCb cb);
void wifiStartMDNS(struct ip_addr);

extern uint8_t wifiState;
extern bool mdns_started;

int wifiGetApCount();
void wifiGetApName(int, char *);
int wifiSignalStrength(int);
void connectToNetwork(char *, char *);
void wifiStartScan();

#endif
