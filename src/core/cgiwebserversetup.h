#ifndef CGIWEBSERVER_H
#define CGIWEBSERVER_H

#include <httpd.h>

bool webServerUploadBusy(void);
unsigned webServerHTMLHeaderLength(void);
int ICACHE_FLASH_ATTR cgiWebServerSetupUpload(HttpdConnData *connData);

#endif /* CGIWEBSERVER_H */
