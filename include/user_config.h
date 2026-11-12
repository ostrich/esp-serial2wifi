#ifndef _USER_CONFIG_H_
#define _USER_CONFIG_H_
#include <c_types.h>
#ifdef __WIN32__
#include <_mingw.h>
#endif

#ifndef SERIAL2WIFI_DEBUG
#define SERIAL2WIFI_DEBUG 0
#endif

#undef SHOW_HEAP_USE
#if SERIAL2WIFI_DEBUG
#define SDK_DBG

#define CMD_DBG
#undef ESPFS_DBG
#undef CGI_DBG
#define CGIFLASH_DBG
#define CGIMQTT_DBG
#define CGIPINS_DBG
#define CGIWIFI_DBG
#define CONFIG_DBG
#define LOG_DBG
#define STATUS_DBG
#define HTTPD_DBG
#define MQTT_DBG
#define MQTTCMD_DBG
#define MQTTCLIENT_DBG
#undef PKTBUF_DBG
#define REST_DBG
#define RESTCMD_DBG
#define SERBR_DBG
#define SERLED_DBG
#define SLIP_DBG
#define UART_DBG
#define MDNS_DBG
#define OPTIBOOT_DBG
#undef SYSLOG_DBG
#define CGISERVICES_DBG
#endif

// CHIP_IN_HOSTNAME is selected by the build configuration.

extern char* firmwareVersion;
extern uint8_t UTILS_StrToIP(const char* str, void *ip);

#endif
