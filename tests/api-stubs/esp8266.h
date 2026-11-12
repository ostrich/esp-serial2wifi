#ifndef API_TEST_SDK
#define API_TEST_SDK
#include "c_types.h"
#include "ip_addr.h"
#include "espconn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define os_strcmp strcmp
#define os_memcmp memcmp
#define os_free free
#define os_zalloc test_zalloc
void *test_zalloc(size_t);
#define os_sprintf sprintf
#define os_strlen strlen
#define os_memset memset
#define os_memcpy memcpy
typedef struct { void (*callback)(void *);void *arg;unsigned ms;bool armed; } ETSTimer;
enum { STATION_IF, SOFTAP_IF };
enum { STATION_IDLE, STATION_CONNECTING, STATION_WRONG_PASSWORD,
  STATION_NO_AP_FOUND, STATION_CONNECT_FAIL, STATION_GOT_IP };
struct station_config { uint8_t ssid[32], password[64], channel,bssid_set,bssid[6];
  struct {int8_t rssi;uint8_t authmode;} threshold;bool open_and_wep_mode_disable,all_channel_scan; };
struct softap_config { uint8_t ssid[32], password[64], ssid_len,channel,authmode,ssid_hidden,max_connection; };
struct ip_info { struct ip_addr ip, netmask, gw; };
struct rst_info { uint32_t reason; };
uint8_t wifi_get_opmode(void);
uint8_t wifi_get_channel(void);
uint8_t wifi_station_get_connect_status(void);
uint8_t wifi_softap_get_station_num(void);
bool wifi_get_ip_info(uint8_t interface, struct ip_info *value);
bool wifi_station_get_config(struct station_config *value);
bool wifi_softap_get_config(struct softap_config *value);
bool wifi_get_macaddr(uint8_t interface, uint8_t *value);
int8_t wifi_station_get_rssi(void);
uint8_t system_upgrade_userbin_check(void);
uint32_t system_get_free_heap_size(void);
struct rst_info *system_get_rst_info(void);
const char *system_get_sdk_version(void);

typedef int STATUS;
enum {OK, ERROR};
enum {REASON_BEACON_TIMEOUT=200,REASON_NO_AP_FOUND=201,REASON_AUTH_FAIL=202,REASON_ASSOC_FAIL=203,REASON_HANDSHAKE_TIMEOUT=204};
enum {AUTH_OPEN,AUTH_WEP,AUTH_WPA_PSK,AUTH_WPA2_PSK,AUTH_WPA_WPA2_PSK};
enum dhcp_status {DHCP_STOPPED,DHCP_STARTED};
#define WIFI_SCAN_TYPE_ACTIVE 0
struct scan_config {unsigned show_hidden,scan_type;struct {struct {unsigned min,max;} active;} scan_time;};
struct bss_info {struct {struct bss_info *stqe_next;} next;uint8_t ssid[32],ssid_len,bssid[6],channel,authmode;int8_t rssi;};
typedef void (*scan_done_cb_t)(void *,STATUS);
void os_timer_disarm(ETSTimer *);
void os_timer_setfn(ETSTimer *,void (*)(void *),void *);
void os_timer_arm(ETSTimer *,unsigned,bool);
uint32_t os_random(void);
bool wifi_set_opmode(uint8_t);
bool wifi_set_opmode_current(uint8_t);
bool wifi_station_get_config_default(struct station_config *);
bool wifi_softap_get_config_default(struct softap_config *);
bool wifi_station_set_config(struct station_config *);
bool wifi_softap_set_config(struct softap_config *);
bool wifi_station_set_hostname(char *);
bool wifi_station_disconnect(void);
bool wifi_station_connect(void);
bool wifi_station_set_reconnect_policy(bool);
bool wifi_set_channel(uint8_t);
uint8_t wifi_station_get_auto_connect(void);
bool wifi_station_set_auto_connect(uint8_t);
struct dhcps_lease {bool enable;struct ip_addr start_ip,end_ip;};
enum dhcp_status wifi_softap_dhcps_status(void);
bool wifi_softap_dhcps_stop(void);
bool wifi_softap_dhcps_start(void);
bool wifi_softap_get_dhcps_lease(struct dhcps_lease *);
bool wifi_softap_set_dhcps_lease(struct dhcps_lease *);
enum dhcp_status wifi_station_dhcpc_status(void);
bool wifi_station_dhcpc_stop(void);
bool wifi_station_dhcpc_start(void);
bool wifi_set_ip_info(uint8_t,struct ip_info *);
void espconn_dns_setserver(uint8_t,ip_addr_t *);
bool wifi_station_scan(struct scan_config *,scan_done_cb_t);
#endif
