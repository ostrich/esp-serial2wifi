#ifndef CGIFLASH_H
#define CGIFLASH_H

#include "httpd.h"

int cgiUploadFirmware(HttpdConnData *connData);
int cgiRebootFirmware(HttpdConnData *connData);
bool firmwareOTASupported(void);
const char *firmwareOTAReason(void);

#endif
