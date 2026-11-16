#ifndef CGI_H
#define CGI_H

#include <esp8266.h>
#include "httpd.h"

void noCacheHeaders(HttpdConnData *connData, int code);
void jsonHeader(HttpdConnData *connData, int code);
void errorResponse(HttpdConnData *connData, int code, char *message);

int cgiMenu(HttpdConnData *connData);

uint8_t UTILS_StrToIP(const char *str, void *ip);

#endif
