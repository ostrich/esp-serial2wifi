/*
* ----------------------------------------------------------------------------
* "THE BEER-WARE LICENSE" (Revision 42):
* Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
* this notice you can do whatever you want with this stuff. If we meet some day,
* and you think this stuff is worth it, you can buy me a beer in return.
* ----------------------------------------------------------------------------
* Heavily modified and enhanced by Thorsten von Eicken in 2015
* ----------------------------------------------------------------------------
*/

#include <esp8266.h>
#include "routes.h"
#include "httpdespfs.h"
#include "cgi.h"
#include "updates.h"
#include "cgioptiboot.h"
#include "cgimega.h"
#include "serial_api.h"
#include "management.h"
#include "log.h"
#include "sdk_rto.h"
#include "api.h"
#include "network.h"
#ifdef WEBSERVER
#include "custom_pages.h"
#endif

/*
This is the main url->function dispatching data struct.
In short, it's a struct with various URLs plus their handlers. The handlers can
be 'standard' CGI functions you wrote, or 'special' CGIs requiring an argument.
They can also be auth-functions. An asterisk will match any url starting with
everything before the asterisks; "*" matches everything. The list will be
handled top-down, so make sure to put more specific rules above the more
general ones.
*/
static int ICACHE_FLASH_ATTR cgiSdkRto(HttpdConnData *c) {
  if (!c->conn)
    return HTTPD_CGI_DONE;
  char buf[1024];
  sdk_rto_json(buf);
  jsonHeader(c, 200);
  httpdSend(c, buf, -1);
  return HTTPD_CGI_DONE;
}

HttpdBuiltInUrl builtInUrls[] = {
    {"/api/v1/capabilities", apiCapabilities, NULL},
    {"/api/v1/status", apiStatus, NULL},
    {"/api/v1/firmware", apiFirmwareInfo, NULL},
    {"/api/v1/firmware/upload", apiFirmwareUpload, NULL},
    {"/api/v1/firmware/activate", apiFirmwareActivate, NULL},
    {"/api/v1/pages", apiPagesInfo, NULL},
    {"/api/v1/pages/upload", apiPagesUpload, NULL},
    {"/api/v1/pages/clear", apiPagesClear, NULL},
    {"/api/v1/pages/*", apiPagesInfo, NULL},
    {"/api/v1/settings", apiSettings, NULL},
    {"/api/v1/integrations", apiIntegrations, NULL},
    {"/api/v1/diagnostics/uart-log", apiDiagnosticsLogMode, NULL},
    {"/api/v1/diagnostics/log", apiDiagnosticsLog, NULL},
    {"/api/v1/settings/description", apiManagementSave, "description"},
    {"/api/v1/settings/pins", apiManagementSave, "pins"},
    {"/api/v1/settings/clock", apiManagementSave, "clock"},
    {"/api/v1/settings/syslog", apiManagementSave, "syslog"},
    {"/api/v1/settings/restart", apiManagementRestart, NULL},
    {"/api/v1/integrations/mqtt", apiManagementSave, "mqtt"},
    {"/api/v1/integrations/reporting", apiManagementSave, "reporting"},
    {"/api/v1/integrations/slip", apiManagementSave, "slip"},
    {"/api/v1/serial", apiSerial, NULL},
    {"/api/v1/serial/live", apiSerialLive, NULL},
    {"/api/v1/serial/capture", apiSerialCapture, NULL},
    {"/api/v1/serial/capture/*", apiSerialCapture, NULL},
    {"/api/v1/serial/uart", apiSerialUART, NULL},
    {"/api/v1/serial/send", apiSerialSend, NULL},
    {"/api/v1/serial/clear", apiSerialClear, NULL},
    {"/api/v1/serial/reset-mcu", apiSerialReset, NULL},
    {"/api/v1/network", apiNetwork, NULL},
    {"/api/v1/network/station", apiNetworkStation, NULL},
    {"/api/v1/network/ap", apiNetworkAP, NULL},
    {"/api/v1/network/identity", apiNetworkIdentity, NULL},
    {"/api/v1/network/scan", apiNetworkScan, NULL},
    {"/api/v1/network/scan/*", apiNetworkScanPage, NULL},
    {"/diag/rto", cgiSdkRto, NULL},
    {"/", cgiRedirect, "/index.html"},
    {"/menu", cgiMenu, NULL},

    {"/pgm/sync", cgiOptibootSync, NULL},
    {"/pgm/upload", cgiOptibootData, NULL},

    {"/pgmmega/sync", cgiMegaSync, NULL},           // Start programming mode
    {"/pgmmega/upload", cgiMegaData, NULL},         // Upload stuff
    {"/pgmmega/read/*", cgiMegaRead, NULL},         // Download stuff (to verify)
    {"/pgmmega/fuse/*", cgiMegaFuse, NULL},         // Read or write fuse
    {"/pgmmega/rebootmcu", cgiMegaRebootMCU, NULL}, // Get out of programming mode

#ifdef WEBSERVER
    {"/user/*", cgiUserFsHook, NULL},
    {"*.json", WEB_CgiJsonHook, NULL}, //Catch-all cgi JSON queries
#endif
    {"*", cgiEspFsHook, NULL}, //Catch-all cgi function for the filesystem
    {NULL, NULL, NULL}};
