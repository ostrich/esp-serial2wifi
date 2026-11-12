/*
Some random cgi routines.
*/

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
#include "cgi.h"
#include "config.h"
#include "api_request.h"
#ifdef WEBSERVER
#include "web-server.h"
#endif

#ifdef CGI_DBG
#define DBG(format, ...)                                                                           \
  do {                                                                                             \
    os_printf(format, ##__VA_ARGS__);                                                              \
  } while (0)
#else
#define DBG(format, ...)                                                                           \
  do {                                                                                             \
  } while (0)
#endif

void ICACHE_FLASH_ATTR noCacheHeaders(HttpdConnData *connData, int code) {
  httpdStartResponse(connData, code);
  httpdHeader(connData, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpdHeader(connData, "Pragma", "no-cache");
  httpdHeader(connData, "Expires", "0");
}

void ICACHE_FLASH_ATTR jsonHeader(HttpdConnData *connData, int code) {
  noCacheHeaders(connData, code);
  httpdHeader(connData, "Content-Type", "application/json");
  httpdEndHeaders(connData);
}

void ICACHE_FLASH_ATTR errorResponse(HttpdConnData *connData, int code, char *message) {
  noCacheHeaders(connData, code);
  httpdEndHeaders(connData);
  httpdSend(connData, message, -1);
  DBG("HTTP %d error response: \"%s\"\n", code, message);
}

uint8_t ICACHE_FLASH_ATTR UTILS_StrToIP(const char *str, void *ip) {
  /* The count of the number of bytes processed. */
  int i;
  /* A pointer to the next digit to process. */
  const char *start;

  start = str;
  for (i = 0; i < 4; i++) {
    /* The digit being processed. */
    char c;
    /* The value of this byte. */
    int n = 0;
    while (1) {
      c = *start;
      start++;
      if (c >= '0' && c <= '9') {
        n *= 10;
        n += c - '0';
      }
      /* We insist on stopping at "." if we are still parsing
      the first, second, or third numbers. If we have reached
      the end of the numbers, we will allow any character. */
      else if ((i < 3 && c == '.') || i == 3) {
        break;
      } else {
        return 0;
      }
    }
    if (n >= 256) {
      return 0;
    }
    ((uint8_t *)ip)[i] = n;
  }
  return 1;
}

extern char *firmwareVersion; // in main.c

static void ICACHE_FLASH_ATTR pageMenu(ApiJson *j, const void *unused) {
  (void)unused;
  apiJsonLiteral(
      j,
      "{\"menu\":[\"Dashboard\",\"/\",\"Network\",\"/index.html#/network\",\"Serial & MCU\",\"/index.html#/serial\",\"Device Settings\",\"/index.html#/settings\",\"Diagnostics\",\"/index.html#/diagnostics\",\"Firmware\",\"/index.html#/firmware\"");
#if defined(MQTT) || defined(REST) || defined(SOCKET)
  apiJsonLiteral(j, ",\"Integrations\",\"/index.html#/integrations\"");
#endif
#ifdef WEBSERVER
  apiJsonLiteral(j, ",\"Custom Pages\",\"/index.html#/pages\"");
  apiJsonRaw(j, WEB_UserPages());
#endif
  apiJsonLiteral(j, "],\"version\":");
  apiJsonString(j, firmwareVersion, 96);
  apiJsonLiteral(j, ",\"name\":");
  apiJsonString(j, flashConfig.hostname, sizeof flashConfig.hostname);
  apiJsonLiteral(j, "}");
}
int ICACHE_FLASH_ATTR cgiMenu(HttpdConnData *connData) {
  if (!connData->conn)
    return HTTPD_CGI_DONE;
  return apiResponse(connData, 200, pageMenu, NULL);
}
