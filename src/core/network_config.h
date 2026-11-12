#ifndef SERIAL2WIFI_NETWORK_CONFIG_H
#define SERIAL2WIFI_NETWORK_CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
bool networkAPValid(uint32_t);
bool networkIP(const char *, uint32_t *);
bool networkStatic(const char *, const char *, const char *, const char *, const char *,
                   uint32_t[5]);
bool networkPassword(const char *, bool);
bool networkHostname(const char *);
bool networkMac(const char *, uint8_t[6]);
#endif
