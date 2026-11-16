#include <esp8266.h>
#include "api.h"
#include "api_json.h"
#include "api_request.h"
#include "cgi.h"
#include "cgiflash.h"
#include "cgiwifi.h"
#include "wifi_recovery.h"
#include "config.h"
#include "flash_layout.h"
#include "status.h"
#include "serbridge.h"
#include "uart.h"
#include "console.h"
#ifdef MQTT
#include "mqtt_client.h"
#endif

extern char *firmwareVersion;

typedef struct {
  uint32_t uptime_seconds, heap, rx_bytes, serial_boot, serial_errors[11];
  uint16_t uptime_millis;
  uint8_t mode, station_state, channel, ap_clients, listeners, serial_clients, reset, slot;
  int8_t rssi;
  bool have_ip, have_ap_ip, loss, ota_eligible;
  char ssid[32], ip[16], ap_ip[16], format[8], mac[18];
  size_t ssid_length;
  const char *wifi_state, *wifi_reason, *mqtt_state;
} ApiSnapshot;

static void ICACHE_FLASH_ATTR ipText(char result[16], struct ip_addr address) {
  const uint8_t *octets = (const uint8_t *)&address.addr;
  os_sprintf(result, "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
}

static void ICACHE_FLASH_ATTR capture(ApiSnapshot *snapshot) {
  struct ip_info address;
  union {
    struct station_config station;
    struct softap_config ap;
  } config;
  UartRxStats uart;
  SerbridgeInputStats bridge;
  uint8_t mac[6];
  os_memset(snapshot, 0, sizeof(*snapshot));
  statusUptime(&snapshot->uptime_seconds, &snapshot->uptime_millis);
  snapshot->heap = system_get_free_heap_size();
  snapshot->ota_eligible = firmwareOTASupported();
  snapshot->mode = wifi_get_opmode() & 3;
  snapshot->channel = wifi_get_channel();
  snapshot->station_state = wifi_station_get_connect_status();
  if (snapshot->station_state != STATION_GOT_IP && wifiRecoveryStatus())
    snapshot->station_state = wifiRecoveryStatus();
  if (snapshot->station_state != STATION_GOT_IP && wifiRecoveryDhcpFailed())
    snapshot->station_state = WIFI_RECOVERY_DHCP_TIMEOUT;
  snapshot->slot = system_upgrade_userbin_check();
  struct rst_info *reset = system_get_rst_info();
  snapshot->reset = reset ? reset->reason : 255;
  snapshot->wifi_state = "unknown";
  snapshot->wifi_reason = NULL;
  if (snapshot->mode & 2) {
    os_memset(&address, 0, sizeof(address));
    if (wifi_get_ip_info(SOFTAP_IF, &address) && address.ip.addr) {
      snapshot->have_ap_ip = true;
      ipText(snapshot->ap_ip, address.ip);
    }
    snapshot->ap_clients = wifi_softap_get_station_num();
  }
  if (snapshot->mode & 1) {
    os_memset(&config, 0, sizeof(config));
    if (wifi_station_get_config(&config.station)) {
      os_memcpy(snapshot->ssid, config.station.ssid, 32);
      while (snapshot->ssid_length < 32 && snapshot->ssid[snapshot->ssid_length])
        snapshot->ssid_length++;
    }
    switch (snapshot->station_state) {
    case STATION_GOT_IP:
      os_memset(&address, 0, sizeof(address));
      if (wifi_get_ip_info(STATION_IF, &address) && address.ip.addr) {
        snapshot->have_ip = true;
        ipText(snapshot->ip, address.ip);
        snapshot->wifi_state = "connected";
        snapshot->rssi = wifi_station_get_rssi();
      } else
        snapshot->wifi_state = "connecting";
      break;
    case STATION_CONNECTING:
      snapshot->wifi_state = "connecting";
      if (wifiState == wifiIsConnected)
        snapshot->wifi_reason = "dhcp_pending";
      break;
    case WIFI_RECOVERY_DHCP_TIMEOUT:
      snapshot->wifi_state = "failed";
      snapshot->wifi_reason = "dhcp_timeout";
      break;
    case STATION_WRONG_PASSWORD:
      snapshot->wifi_state = "failed";
      snapshot->wifi_reason = "wrong_password";
      break;
    case STATION_NO_AP_FOUND:
      snapshot->wifi_state = "failed";
      snapshot->wifi_reason = "ap_not_found";
      break;
    case STATION_CONNECT_FAIL:
      snapshot->wifi_state = "failed";
      snapshot->wifi_reason = "association_failed";
      break;
    }
  } else if (snapshot->mode == 2) {
    os_memset(&config, 0, sizeof(config));
    if (wifi_softap_get_config(&config.ap)) {
      snapshot->ssid_length = config.ap.ssid_len <= 32 ? config.ap.ssid_len : 32;
      os_memcpy(snapshot->ssid, config.ap.ssid, snapshot->ssid_length);
    }
    snapshot->wifi_state = snapshot->have_ap_ip ? "ready" : "unknown";
  } else
    snapshot->wifi_state = "disabled";
  if (!snapshot->have_ip && snapshot->mode == 2 && snapshot->have_ap_ip) {
    snapshot->have_ip = true;
    os_memcpy(snapshot->ip, snapshot->ap_ip, sizeof(snapshot->ip));
  }
  os_memset(mac, 0, sizeof(mac));
  if (wifi_get_macaddr(snapshot->mode == 2 ? SOFTAP_IF : STATION_IF, mac))
    os_sprintf(snapshot->mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5]);
  snapshot->listeners = serbridge_listener_mask();
  snapshot->serial_clients = serbridge_client_count();
  uart_get_rx_stats(&uart);
  serbridge_get_input_stats(&bridge);
  snapshot->rx_bytes = uart.bytes_received;
  snapshot->serial_boot = consoleBootId();
  uint32_t errors[] = {uart.ring_drops,     uart.framing_drops,      uart.framing_errors,
                       uart.fifo_overruns,  bridge.net_drops,        bridge.rejected,
                       bridge.alloc_errors, bridge.net_alloc_errors, bridge.net_send_errors,
                       bridge.hold_errors,  consoleStreamTotalLost()};
  os_memcpy(snapshot->serial_errors, errors, sizeof(errors));
  // Byte drops and FIFO overruns establish loss. Other error events do not.
  snapshot->loss = errors[0] || errors[1] || errors[3] || errors[4] || errors[10];
  if (flashConfig.data_bits >= FIVE_BITS && flashConfig.data_bits <= EIGHT_BITS &&
      (flashConfig.parity == NONE_BITS || flashConfig.parity == EVEN_BITS ||
       flashConfig.parity == ODD_BITS) &&
      (flashConfig.stop_bits == ONE_STOP_BIT || flashConfig.stop_bits == TWO_STOP_BIT ||
       flashConfig.stop_bits == ONE_HALF_STOP_BIT)) {
    snapshot->format[0] = '5' + flashConfig.data_bits;
    snapshot->format[1] = flashConfig.parity == NONE_BITS  ? 'N'
                          : flashConfig.parity == ODD_BITS ? 'O'
                                                           : 'E';
    if (flashConfig.stop_bits == ONE_HALF_STOP_BIT)
      os_memcpy(snapshot->format + 2, "1.5", 4);
    else
      snapshot->format[2] = flashConfig.stop_bits == ONE_STOP_BIT ? '1' : '2';
  }
#ifdef MQTT
  if (!flashConfig.mqtt_enable)
    snapshot->mqtt_state = "disabled";
  else if (mqttClient.connack_received && mqttClient.connack_code != 0)
    snapshot->mqtt_state = "failed";
  else if (mqttClient.connState == MQTT_CONNECTED && mqttClient.connack_received)
    snapshot->mqtt_state = mqttClient.connack_code == 0 ? "connected" : "failed";
  else if (mqttClient.connState == MQTT_CONNECTED || mqttClient.connState == TCP_CONNECTING ||
           mqttClient.connState == TCP_RECONNECT_REQ)
    snapshot->mqtt_state = "connecting";
  else
    snapshot->mqtt_state = "unknown";
#endif
}

static const char *ICACHE_FLASH_ATTR resetReason(uint8_t code) {
  switch (code) {
  case 0:
    return "power_on";
  case 1:
    return "hardware_watchdog";
  case 2:
    return "exception";
  case 3:
    return "software_watchdog";
  case 4:
    return "software_restart";
  case 5:
    return "deep_sleep";
  case 6:
    return "external_reset";
  default:
    return "unknown";
  }
}

static void ICACHE_FLASH_ATTR formatStatus(ApiJson *json, const void *data) {
  const ApiSnapshot *s = data;
  apiJsonLiteral(json, "{\"schema\":1,\"hostname\":");
  apiJsonString(json, flashConfig.hostname, sizeof(flashConfig.hostname));
  apiJsonLiteral(json, ",\"description\":");
  apiJsonString(json, flashConfig.sys_descr, sizeof(flashConfig.sys_descr));
  apiJsonLiteral(json, ",\"firmware\":{\"version\":");
  apiJsonString(json, firmwareVersion, 96);
  apiJsonLiteral(json, ",\"sdk\":");
  apiJsonString(json, system_get_sdk_version(), 48);
  apiJsonLiteral(json, ",\"slot\":");
  apiJsonString(json, s->slot == 0 ? "user1" : s->slot == 1 ? "user2" : NULL, 8);
  apiJsonLiteral(json, ",\"image_abi\":");
  apiJsonString(json, SERIAL2WIFI_IMAGE_ABI, 48);
  apiJsonLiteral(json, ",\"flash_bytes\":");
  apiJsonUnsigned(json, FLASH_LAYOUT_BYTES);
  apiJsonLiteral(json, ",\"ota_eligible\":");
  apiJsonBool(json, s->ota_eligible);
  apiJsonLiteral(json, "},\"system\":{\"uptime_ms\":");
  apiJsonMillis(json, s->uptime_seconds, s->uptime_millis);
  apiJsonLiteral(json, ",\"heap_free_bytes\":");
  apiJsonUnsigned(json, s->heap);
  apiJsonLiteral(json, ",\"reset_reason\":");
  apiJsonString(json, resetReason(s->reset), 32);
  apiJsonLiteral(json, ",\"reset_code\":");
  if (s->reset == 255)
    apiJsonLiteral(json, "null");
  else
    apiJsonUnsigned(json, s->reset);
  apiJsonLiteral(json, "},\"wifi\":{\"mode\":");
  apiJsonString(json,
                s->mode == 2   ? "ap"
                : s->mode == 1 ? "station"
                : s->mode == 3 ? "station_ap"
                               : "disabled",
                16);
  apiJsonLiteral(json, ",\"state\":");
  apiJsonString(json, s->wifi_state, 16);
  apiJsonLiteral(json, ",\"reason\":");
  apiJsonString(json, s->wifi_reason, 32);
  apiJsonLiteral(json, ",\"ssid\":");
  if (s->ssid_length)
    apiJsonStringN(json, s->ssid, s->ssid_length);
  else
    apiJsonLiteral(json, "null");
  apiJsonLiteral(json, ",\"ip\":");
  apiJsonString(json, s->have_ip ? s->ip : NULL, 16);
  apiJsonLiteral(json, ",\"ap_ip\":");
  apiJsonString(json, s->have_ap_ip ? s->ap_ip : NULL, 16);
  apiJsonLiteral(json, ",\"mac\":");
  apiJsonString(json, s->mac[0] ? s->mac : NULL, sizeof(s->mac));
  apiJsonLiteral(json, ",\"rssi_dbm\":");
  if ((s->mode & 1) && s->station_state == STATION_GOT_IP && s->rssi < 0)
    apiJsonSigned(json, s->rssi);
  else
    apiJsonLiteral(json, "null");
  apiJsonLiteral(json, ",\"channel\":");
  if (s->mode && s->channel >= 1 && s->channel <= 14)
    apiJsonUnsigned(json, s->channel);
  else
    apiJsonLiteral(json, "null");
  apiJsonLiteral(json, ",\"ap_clients\":");
  if (s->mode & 2)
    apiJsonUnsigned(json, s->ap_clients);
  else
    apiJsonLiteral(json, "null");
  apiJsonLiteral(json, "},\"serial\":{\"state\":");
  // Current listener readiness is separate from the cumulative error history.
  apiJsonString(json, !(s->listeners & 1) ? "failed" : "ready", 16);
  apiJsonLiteral(json, ",\"listener_ready\":");
  apiJsonBool(json, s->listeners & 1);
  apiJsonLiteral(json, ",\"clients\":");
  apiJsonUnsigned(json, s->serial_clients);
  apiJsonLiteral(json, ",\"baud\":");
  if (flashConfig.baud_rate > 0)
    apiJsonUnsigned(json, flashConfig.baud_rate);
  else
    apiJsonLiteral(json, "null");
  apiJsonLiteral(json, ",\"configured_format\":");
  apiJsonString(json, s->format[0] ? s->format : NULL, sizeof(s->format));
  apiJsonLiteral(json, ",\"target_connected\":null,\"rx_bytes\":");
  apiJsonUnsigned(json, s->rx_bytes);
  apiJsonLiteral(json, ",\"loss_detected\":");
  apiJsonBool(json, s->loss);
  apiJsonLiteral(json, ",\"boot_id\":");
  apiJsonUnsigned(json, s->serial_boot);
  apiJsonLiteral(json, ",\"errors\":{");
  static const char *names[] = {"uart_ring_bytes", "uart_frame_bytes", "uart_frames",
                                "uart_overruns",   "tcp_output_bytes", "tcp_rejections",
                                "tcp_input_alloc", "tcp_output_alloc", "tcp_send",
                                "tcp_hold",        "web_output_bytes"};
  for (unsigned i = 0; i < 11; i++) {
    if (i)
      apiJsonLiteral(json, ",");
    apiJsonString(json, names[i], 24);
    apiJsonLiteral(json, ":");
    apiJsonUnsigned(json, s->serial_errors[i]);
  }
  apiJsonLiteral(json, "}");
  apiJsonLiteral(json, "}");
#ifdef MQTT
  apiJsonLiteral(json, ",\"mqtt\":{\"enabled\":");
  apiJsonBool(json, flashConfig.mqtt_enable);
  apiJsonLiteral(json, ",\"state\":");
  apiJsonString(json, s->mqtt_state, 16);
  apiJsonLiteral(json, "}");
#endif
  apiJsonLiteral(json, "}");
}

int ICACHE_FLASH_ATTR apiStatus(HttpdConnData *conn) {
  if (!apiRead(conn))
    return HTTPD_CGI_DONE;
  ApiSnapshot snapshot;
  capture(&snapshot);
  return apiResponse(conn, 200, formatStatus, &snapshot);
}

typedef struct {
  bool ota;
  uint8_t mask;
} Capabilities;
static void ICACHE_FLASH_ATTR formatCapabilities(ApiJson *json, const void *data) {
  const Capabilities *c = data;
  bool ota = c->ota;
  apiJsonLiteral(json, "{\"schema\":1,\"features\":{\"mqtt\":");
#ifdef MQTT
  apiJsonBool(json, 1);
#else
  apiJsonBool(json, 0);
#endif
  apiJsonLiteral(json, ",\"syslog\":");
#ifdef SYSLOG
  apiJsonBool(json, 1);
#else
  apiJsonBool(json, 0);
#endif
  apiJsonLiteral(json, ",\"custom_pages\":");
#ifdef WEBSERVER
  apiJsonBool(json, 1);
#else
  apiJsonBool(json, 0);
#endif
  apiJsonLiteral(json, ",\"rest\":");
#ifdef REST
  apiJsonBool(json, 1);
#else
  apiJsonBool(json, 0);
#endif
  apiJsonLiteral(json, ",\"socket\":");
#ifdef SOCKET
  apiJsonBool(json, 1);
#else
  apiJsonBool(json, 0);
#endif
  apiJsonLiteral(json, ",\"ota\":");
  apiJsonBool(json, ota);
  apiJsonLiteral(
      json,
      ",\"optiboot_upload\":true,\"optiboot_readback\":true},\"limits\":{\"console_buffer_bytes\":");
  apiJsonUnsigned(json, CONSOLE_BUFFER_BYTES);
  apiJsonLiteral(json, ",\"max_ota_image_bytes\":");
  apiJsonUnsigned(json, ota ? FLASH_APP1_SIZE : 0);
  apiJsonLiteral(json, ",\"flash_slot_bytes\":");
  apiJsonUnsigned(json, FLASH_APP1_SIZE);
  apiJsonLiteral(json, "},\"serial_tcp_ports\":[");
  uint8_t mask = c->mask;
  if (mask & 1)
    apiJsonLiteral(json, "23");
  if (mask & 2)
    apiJsonRaw(json, (mask & 1) ? ",2323" : "2323");
  apiJsonLiteral(json, "]}");
}
int ICACHE_FLASH_ATTR apiCapabilities(HttpdConnData *conn) {
  if (!apiRead(conn))
    return HTTPD_CGI_DONE;
  Capabilities data = {firmwareOTASupported(), serbridge_listener_mask()};
  return apiResponse(conn, 200, formatCapabilities, &data);
}
