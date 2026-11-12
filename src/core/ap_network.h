#ifndef SERIAL2WIFI_AP_NETWORK_H
#define SERIAL2WIFI_AP_NETWORK_H
#include <stdint.h>
#include <stdbool.h>
uint32_t apNetworkAddress(void);
bool apNetworkConfigure(uint32_t address, bool persist);
void apNetworkInit(void);
#endif
