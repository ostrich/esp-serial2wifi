#include <esp8266.h>
#include "wifi_setup.h"

// Only called when no current-project configuration record was restored.
// No shared password, station credentials or board-specific identity is embedded.
bool ICACHE_FLASH_ATTR wifiSetupDefaults(void) {
  struct softap_config setup;
  os_memset(&setup, 0, sizeof setup);
  os_snprintf((char *)setup.ssid, sizeof setup.ssid, "ESP-Serial2WiFi-%06x",
              system_get_chip_id() & 0xffffff);
  setup.ssid_len = os_strlen((char *)setup.ssid);
  setup.channel = 1;
  setup.authmode = AUTH_OPEN;
  setup.max_connection = 4;
  setup.beacon_interval = 100;
  return wifi_set_opmode(SOFTAP_MODE) && wifi_softap_set_config(&setup);
}
