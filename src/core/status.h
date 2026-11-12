#ifndef STATUS_H
#define STATUS_H
#include <stdint.h>

int mqttStatusMsg(char *buf);
void statusWifiUpdate(uint8_t state);
void statusInit(void);
void statusUptime(uint32_t *seconds, uint16_t *millis);

#endif
