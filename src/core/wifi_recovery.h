#ifndef SERIAL2WIFI_WIFI_RECOVERY_H
#define SERIAL2WIFI_WIFI_RECOVERY_H
#define WIFI_RECOVERY_DHCP_TIMEOUT 6
void wifiRecoveryDhcpTimeout(void);
bool wifiRecoveryDhcpFailed(void);
bool wifiRecoveryBegin(void);
void wifiRecoveryConnected(void);
void wifiRecoveryObserve(uint8_t reason);
void wifiRecoveryAssociated(void);
bool wifiRecoveryPoll(uint8_t station_state);
uint8_t wifiRecoveryStatus(void);
#endif
