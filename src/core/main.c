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
#include "httpd.h"
#include "httpdespfs.h"
#include "cgi.h"
#include "cgiwifi.h"
#include "cgiflash.h"
#include "updates.h"
#include "cgioptiboot.h"
#include "cgimega.h"
#include "cgiwebserversetup.h"
#include "espfs.h"
#include "uart.h"
#include "serbridge.h"
#include "status.h"
#include "serled.h"
#include "console.h"
#include "serial_api.h"
#include "management.h"
#include "config.h"
#include "flash_layout.h"
#include "log.h"
#include "gpio.h"
#include "time_service.h"
#include "sdk_rto.h"
#include "api.h"
#include "network.h"
#include "wifi_setup.h"

#ifdef WEBSERVER
#include "web-server.h"
#endif

#ifdef SYSLOG
#include "syslog.h"
#define NOTICE(format, ...) do {                                            \
  LOG_NOTICE(format, ## __VA_ARGS__ );                                      \
  os_printf(format "\n", ## __VA_ARGS__);                                   \
} while ( 0 )
#else
#define NOTICE(format, ...) do {                                            \
  os_printf(format "\n", ## __VA_ARGS__);                                   \
} while ( 0 )
#endif

#ifdef MEMLEAK_DEBUG
#include "mem.h"
bool ICACHE_FLASH_ATTR check_memleak_debug_enable(void)
{
    return MEMLEAK_DEBUG_ENABLE;
}
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
  if (!c->conn) return HTTPD_CGI_DONE;
  char buf[1024];
  sdk_rto_json(buf);
  jsonHeader(c, 200);
  httpdSend(c, buf, -1);
  return HTTPD_CGI_DONE;
}

HttpdBuiltInUrl builtInUrls[] = {
  { "/api/v1/capabilities", apiCapabilities, NULL },
  { "/api/v1/status", apiStatus, NULL },
  { "/api/v1/firmware", apiFirmwareInfo, NULL },
  { "/api/v1/firmware/upload", apiFirmwareUpload, NULL },
  { "/api/v1/firmware/activate", apiFirmwareActivate, NULL },
  { "/api/v1/pages", apiPagesInfo, NULL },
  { "/api/v1/pages/upload", apiPagesUpload, NULL },
  { "/api/v1/pages/clear", apiPagesClear, NULL },
  { "/api/v1/pages/*", apiPagesInfo, NULL },
  { "/api/v1/settings", apiSettings, NULL },
  { "/api/v1/integrations", apiIntegrations, NULL },
  { "/api/v1/diagnostics/uart-log", apiDiagnosticsLogMode, NULL },
  { "/api/v1/diagnostics/log", apiDiagnosticsLog, NULL },
  { "/api/v1/settings/description", apiManagementSave, "description" },
  { "/api/v1/settings/pins", apiManagementSave, "pins" },
  { "/api/v1/settings/clock", apiManagementSave, "clock" },
  { "/api/v1/settings/syslog", apiManagementSave, "syslog" },
  { "/api/v1/settings/restart", apiManagementRestart, NULL },
  { "/api/v1/integrations/mqtt", apiManagementSave, "mqtt" },
  { "/api/v1/integrations/reporting", apiManagementSave, "reporting" },
  { "/api/v1/integrations/slip", apiManagementSave, "slip" },
  { "/api/v1/serial", apiSerial, NULL },
  { "/api/v1/serial/live", apiSerialLive, NULL },
  { "/api/v1/serial/capture", apiSerialCapture, NULL },
  { "/api/v1/serial/capture/*", apiSerialCapture, NULL },
  { "/api/v1/serial/uart", apiSerialUART, NULL },
  { "/api/v1/serial/send", apiSerialSend, NULL },
  { "/api/v1/serial/clear", apiSerialClear, NULL },
  { "/api/v1/serial/reset-mcu", apiSerialReset, NULL },
  { "/api/v1/network", apiNetwork, NULL },
  { "/api/v1/network/station", apiNetworkStation, NULL },
  { "/api/v1/network/ap", apiNetworkAP, NULL },
  { "/api/v1/network/identity", apiNetworkIdentity, NULL },
  { "/api/v1/network/scan", apiNetworkScan, NULL },
  { "/api/v1/network/scan/*", apiNetworkScanPage, NULL },
  { "/diag/rto", cgiSdkRto, NULL },
  { "/", cgiRedirect, "/index.html" },
  { "/menu", cgiMenu, NULL },

  { "/pgm/sync", cgiOptibootSync, NULL },
  { "/pgm/upload", cgiOptibootData, NULL },

  { "/pgmmega/sync", cgiMegaSync, NULL },		// Start programming mode
  { "/pgmmega/upload", cgiMegaData, NULL },		// Upload stuff
  { "/pgmmega/read/*", cgiMegaRead, NULL },		// Download stuff (to verify)
  { "/pgmmega/fuse/*", cgiMegaFuse, NULL },		// Read or write fuse
  { "/pgmmega/rebootmcu", cgiMegaRebootMCU, NULL },	// Get out of programming mode

#ifdef WEBSERVER
  { "/user/*", cgiUserFsHook, NULL },
  { "*.json", WEB_CgiJsonHook, NULL }, //Catch-all cgi JSON queries
#endif
  { "*", cgiEspFsHook, NULL }, //Catch-all cgi function for the filesystem
  { NULL, NULL, NULL }
};

#ifdef SHOW_HEAP_USE
static ETSTimer prHeapTimer;
static void ICACHE_FLASH_ATTR prHeapTimerCb(void *arg) {
  os_printf("Heap: %ld\n", (unsigned long)system_get_free_heap_size());
}
#endif

# define VERS_STR_STR(V) #V
# define VERS_STR(V) VERS_STR_STR(V)
char* firmwareVersion = VERS_STR(VERSION);

// address of espfs binary blob
#include "ui_storage.h"
extern uint32_t _binary_espfs_img_start;

extern void mqtt_client_init(void);

void ICACHE_FLASH_ATTR
user_rf_pre_init(void) {
  //default is enabled
  system_set_os_print(DEBUG_SDK);
}

/* user_rf_cal_sector_set is a required function that is called by the SDK to get a flash
 * sector number where it can store RF calibration data. This was introduced with SDK 1.5.4.1
 * and is necessary because Espressif ran out of pre-reserved flash sectors. Ooops...  */
uint32 ICACHE_FLASH_ATTR
user_rf_cal_sector_set(void) {
  return FLASH_RF_START / SPI_FLASH_SEC_SIZE;
}

// Register ownership before the SDK initializes RF or persistent system data.
void ICACHE_FLASH_ATTR user_pre_init(void) {
  if (!flashLayoutRegister()) {
    os_printf("Flash geometry/partition registration failed; recover over serial\n");
    while (1) { } // Never continue into SDK writes with an invalid table.
  }
}

// Main routine to initialize ESP Serial2WiFi.
void ICACHE_FLASH_ATTR
user_init(void) {
  // uncomment the following three lines to see flash config messages for troubleshooting
  //uart_init(115200, 115200);
  //logInit();
  //os_delay_us(100000L);
  // get the flash config so we know how to init things
  //configWipe(); // uncomment to reset the config for testing purposes
  bool restoreOk = configRestore();
  // Init gpio pin registers
  gpio_init();
  gpio_output_set(0, 0, 0, (1<<15)); // some people tie it to GND, gotta ensure it's disabled
  // init UART
  uart_init(CALC_UARTMODE(flashConfig.data_bits, flashConfig.parity, flashConfig.stop_bits),
            flashConfig.baud_rate, 115200);
  logInit(); // must come after init of uart
  // Say hello (leave some time to cause break in TX after boot loader's msg
  os_delay_us(10000L);
  os_printf("\n\n** %s\n", firmwareVersion);
  os_printf("%s\n", restoreOk ? "Loaded saved settings" : "Using initial settings");
  // Status LEDs
  statusInit();
  serledInit();
  // Wifi
  bool defaultsReady = restoreOk || wifiSetupDefaults();
  wifiInit();
  if (!restoreOk && defaultsReady && !configSave())
    os_printf("Could not save initial settings; setup will run again after restart\n");
  // init the flash filesystem with the html stuff
  espFsInit(firmwareFsCtx, &_binary_espfs_img_start, ESPFS_MEMORY);
#if FLASH_EXTERNAL_UI
  // A failed/mismatched bank leaves the embedded USB/OTA recovery page mounted.
  uiStorageMount();
#endif

  //EspFsInitResult res = espFsInit(&_binary_espfs_img_start);
  //os_printf("espFsInit %s\n", res?"ERR":"ok");
  // mount the http handlers
  httpdInit(builtInUrls, 80);
#ifdef WEBSERVER
  WEB_Init();
#endif

  // init the wifi-serial transparent bridge (port 23)
  serbridgeInit(23, 2323);
  uart_add_recv_cb(&serbridgeUartCb);
#ifdef SHOW_HEAP_USE
  os_timer_disarm(&prHeapTimer);
  os_timer_setfn(&prHeapTimer, prHeapTimerCb, NULL);
  os_timer_arm(&prHeapTimer, 10 * 1000, 1);
#endif

  struct rst_info *rst_info = system_get_rst_info();
  NOTICE("Reset cause: %d", rst_info->reason);
  NOTICE("exccause=%d epc1=0x%x epc2=0x%x epc3=0x%x excvaddr=0x%x depc=0x%x",
    rst_info->exccause, rst_info->epc1, rst_info->epc2, rst_info->epc3,
    rst_info->excvaddr, rst_info->depc);
  uint32_t fid = spi_flash_get_id();
  NOTICE("Flash map %d, manuf 0x%02X chip 0x%04X", system_get_flash_size_map(),
      fid & 0xff, (fid&0xff00)|((fid>>16)&0xff));
  NOTICE("** %s: ready, heap=%ld", firmwareVersion, (unsigned long)system_get_free_heap_size());

  // Init SNTP service
  timeServiceInit();
#ifdef MQTT
  if (flashConfig.mqtt_enable) {
    NOTICE("initializing MQTT");
    mqtt_client_init();
  }
#endif
  NOTICE("Waiting for work to do...");
#ifdef MEMLEAK_DEBUG
  system_show_malloc();
#endif
}
