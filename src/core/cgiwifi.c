/*
Cgi/template routines for the /wifi url.
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
#include "cgiwifi.h"
#include "cgi.h"
#include "status.h"
#include "config.h"
#include "ap_network.h"
#include "log.h"
#include "wifi_recovery.h"

#ifdef CGIWIFI_DBG
#define DBG(format, ...) do { os_printf(format, ## __VA_ARGS__); } while(0)
#else
#define DBG(format, ...) do { } while(0)
#endif

# define VERS_STR_STR(V) #V
# define VERS_STR(V) VERS_STR_STR(V)

bool mdns_started = false;
static bool networkPaused,recoveryScheduled;
static void scheduleRecovery(void);

// ===== wifi status change callbacks
static WifiStateChangeCb wifi_state_change_cb[4];

// Temp store for new station config
struct station_config stconf;

// Temp store for new ap config
struct softap_config apconf;

uint8_t wifiState = wifiIsDisconnected;
// reasons for which a connection failed
uint8_t wifiReason = 0;
void (*wifiStatusCb)(uint8_t); // callback when wifi status changes

// handler for wifi status change callback coming in from espressif library
static void ICACHE_FLASH_ATTR wifiHandleEventCb(System_Event_t *evt) {
  switch (evt->event) {
  case EVENT_STAMODE_CONNECTED:
    wifiRecoveryAssociated();
    wifiState = wifiIsConnected;
    wifiReason = 0;
    DBG("Wifi connected to ssid %s, ch %d\n", evt->event_info.connected.ssid,
      evt->event_info.connected.channel);
    statusWifiUpdate(wifiState);
    break;
  case EVENT_STAMODE_DISCONNECTED:
    wifiState = wifiIsDisconnected;
    wifiReason = evt->event_info.disconnected.reason;
    wifiRecoveryObserve(wifiReason);
    DBG("Wi-Fi disconnected from ssid %s, reason %d\n",
      evt->event_info.disconnected.ssid,evt->event_info.disconnected.reason);
    statusWifiUpdate(wifiState);
    scheduleRecovery();
    break;
  case EVENT_STAMODE_DHCP_TIMEOUT:
    wifiRecoveryDhcpTimeout();
    scheduleRecovery();
    break;
  case EVENT_STAMODE_AUTHMODE_CHANGE:
    DBG("Wifi auth mode: %d -> %d\n",
      evt->event_info.auth_change.old_mode, evt->event_info.auth_change.new_mode);
    break;
  case EVENT_STAMODE_GOT_IP:
    wifiRecoveryConnected();
    wifiState = wifiGotIP;
    wifiReason = 0;
    DBG("Wifi got ip:" IPSTR ",mask:" IPSTR ",gw:" IPSTR "\n",
      IP2STR(&evt->event_info.got_ip.ip), IP2STR(&evt->event_info.got_ip.mask),
      IP2STR(&evt->event_info.got_ip.gw));
    statusWifiUpdate(wifiState);
    if (!mdns_started)
      wifiStartMDNS(evt->event_info.got_ip.ip);
    break;
  case EVENT_SOFTAPMODE_STACONNECTED:
    DBG("Wifi AP: station " MACSTR " joined, AID = %d\n",
        MAC2STR(evt->event_info.sta_connected.mac), evt->event_info.sta_connected.aid);
    break;
  case EVENT_SOFTAPMODE_STADISCONNECTED:
    DBG("Wifi AP: station " MACSTR " left, AID = %d\n",
        MAC2STR(evt->event_info.sta_disconnected.mac), evt->event_info.sta_disconnected.aid);
    break;
  default:
    break;
  }

  for (int i = 0; i < 4; i++) {
    if (wifi_state_change_cb[i] != NULL) (wifi_state_change_cb[i])(wifiState);
  }
}

void ICACHE_FLASH_ATTR wifiAddStateChangeCb(WifiStateChangeCb cb) {
  for (int i = 0; i < 4; i++) {
    if (wifi_state_change_cb[i] == cb) return;
    if (wifi_state_change_cb[i] == NULL) {
      wifi_state_change_cb[i] = cb;
      return;
    }
  }
  DBG("WIFI: max state change cb count exceeded\n");
}

static struct mdns_info *mdns_info;

void ICACHE_FLASH_ATTR wifiStartMDNS(struct ip_addr ip) {
  if (flashConfig.mdns_enable) {
    wifi_set_broadcast_if(wifi_get_opmode());
    if (mdns_info == NULL)
      mdns_info = (struct mdns_info *)os_zalloc(sizeof(struct mdns_info));

    if(!mdns_info)return;
    mdns_info->host_name = flashConfig.hostname;
    mdns_info->server_name = flashConfig.mdns_servername;
    mdns_info->server_port = 80;
    mdns_info->ipAddr = ip.addr;
    espconn_mdns_init(mdns_info);
  }
  else {
    espconn_mdns_server_unregister();
    espconn_mdns_close();
    if (mdns_info != NULL) {
      os_free(mdns_info);
      mdns_info = NULL;
    }
  }
  mdns_started = true;
}

// ===== wifi scanning

//WiFi access point data
typedef struct {
  char ssid[32];
  sint8 rssi;
  char enc;
} ApData;

//Scan result
typedef struct {
  char scanInProgress; //if 1, don't access the underlying stuff from the webpage.
  ApData **apData;
  int noAps;
} ScanResultData;

//Static scan status storage.
static ScanResultData cgiWifiAps;

//Callback the code calls when a wlan ap scan is done. Basically stores the result in
//the cgiWifiAps struct.
void ICACHE_FLASH_ATTR wifiScanDoneCb(void *arg, STATUS status) {
  int n;
  struct bss_info *bss_link = (struct bss_info *)arg;

  if (status!=OK) {
    DBG("wifiScanDoneCb status=%d\n", status);
    cgiWifiAps.scanInProgress=0;
    return;
  }

  //Clear prev ap data if needed.
  if (cgiWifiAps.apData!=NULL) {
    for (n=0; n<cgiWifiAps.noAps; n++) os_free(cgiWifiAps.apData[n]);
    os_free(cgiWifiAps.apData);
  }
  cgiWifiAps.apData=NULL;cgiWifiAps.noAps=0;

  //Count amount of access points found.
  n=0;
  while (bss_link != NULL && n<16) {
    bss_link = bss_link->next.stqe_next;
    n++;
  }
  //Allocate memory for access point data
  cgiWifiAps.apData=(ApData **)os_malloc(sizeof(ApData *)*n);
  if(!cgiWifiAps.apData){cgiWifiAps.scanInProgress=0;return;}
  cgiWifiAps.noAps=n;
  DBG("Scan done: found %d APs\n", n);

  //Copy access point data to the static struct
  n=0;
  bss_link = (struct bss_info *)arg;
  while (bss_link != NULL) {
    if (n>=cgiWifiAps.noAps) {
      //This means the bss_link changed under our nose. Shouldn't happen!
      //Break because otherwise we will write in unallocated memory.
      DBG("Huh? I have more than the allocated %d aps!\n", cgiWifiAps.noAps);
      break;
    }
    //Save the ap data.
    cgiWifiAps.apData[n]=(ApData *)os_malloc(sizeof(ApData));
    if(!cgiWifiAps.apData[n]){cgiWifiAps.noAps=n;break;}
    cgiWifiAps.apData[n]->rssi=bss_link->rssi;
    cgiWifiAps.apData[n]->enc=bss_link->authmode;
    strncpy(cgiWifiAps.apData[n]->ssid, (char*)bss_link->ssid, 32);
    DBG("bss%d: %s (%d)\n", n+1, (char*)bss_link->ssid, bss_link->rssi);

    bss_link = bss_link->next.stqe_next;
    n++;
  }
  //We're done.
  cgiWifiAps.scanInProgress=0;
}

static ETSTimer scanTimer;
static void ICACHE_FLASH_ATTR scanStartCb(void *arg) {
  DBG("Starting a scan\n");
  wifi_station_scan(NULL, wifiScanDoneCb);
}

// Start scanning, without parameters
void ICACHE_FLASH_ATTR wifiStartScan() {
  if (!cgiWifiAps.scanInProgress) {
    cgiWifiAps.scanInProgress = 1;
    os_timer_disarm(&scanTimer);
    os_timer_setfn(&scanTimer, scanStartCb, NULL);
    os_timer_arm(&scanTimer, 200, 0);
  }
}

// ===== timers to change state and rescue from failed associations

// reset timer changes back to STA+AP if we can't associate
#define RESET_TIMEOUT (15000) // 15 seconds
static ETSTimer resetTimer;

// This routine is ran some time after a connection attempt to an access point. If
// the connect succeeds, this gets the module in STA-only mode. If it fails, it ensures
// that the module is in STA+AP mode so the user has a chance to recover.
static void ICACHE_FLASH_ATTR resetTimerCb(void *arg) {
  recoveryScheduled=false;
  int x = wifi_station_get_connect_status();
  int m = wifi_get_opmode() & 0x3;
  DBG("Wi-Fi check: mode=%d status=%d\n",m,x);

  if (m == 2) return; // 2=AP, in AP-only mode we don't do any auto-switching

  if ( x == STATION_GOT_IP ) {
    // if we got an IP we could switch to STA-only...
    if (m != 1) { // 1=STA
#ifdef CHANGE_TO_STA
      // We're happily connected, go to STA mode
      DBG("Wifi got IP. Going into STA mode..\n");
      wifi_set_opmode(1);
      os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);recoveryScheduled=true; // check one more time after switching to STA-only
#endif
    }
    log_uart(false);
    // A later disconnect event schedules another bounded recovery check.
  } else {
    // we don't have an IP address
    if (m != 3) {
      DBG("Wifi connect failed. Going into STA+AP mode..\n");
      wifi_set_opmode(3);
      wifi_softap_set_config(&apconf);
    }
    if(wifiRecoveryPoll(x)){log_uart(true);return;}
    log_uart(true);
    DBG("Enabling/continuing uart log\n");
    os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);recoveryScheduled=true;
  }
}

static void ICACHE_FLASH_ATTR scheduleRecovery(void) {
  if(networkPaused||recoveryScheduled||wifiRecoveryStatus())return;
  os_timer_disarm(&resetTimer);os_timer_setfn(&resetTimer,resetTimerCb,NULL);os_timer_arm(&resetTimer,RESET_TIMEOUT,0);recoveryScheduled=true;
}

// Reassociate timer to delay change of association so the original request can finish
static ETSTimer reassTimer;

// Callback actually doing reassociation
static void ICACHE_FLASH_ATTR reassTimerCb(void *arg) {
  DBG("Wifi changing association\n");
  wifi_station_disconnect();
  wifiRecoveryBegin();
  stconf.bssid_set = 0;
  wifi_station_set_config(&stconf);
  wifi_station_connect();
  // Schedule check, we give some extra time (4x) 'cause the reassociation can cause the AP
  // to have to change channel, and then the client needs to follow before it can see the
  // IP address
  os_timer_disarm(&resetTimer);
  os_timer_setfn(&resetTimer, resetTimerCb, NULL);
  os_timer_arm(&resetTimer, 4*RESET_TIMEOUT, 0);recoveryScheduled=true;
}

void ICACHE_FLASH_ATTR wifiNetworkPause(void) {
  networkPaused=true;recoveryScheduled=false;
  os_timer_disarm(&resetTimer); os_timer_disarm(&reassTimer);
}
void ICACHE_FLASH_ATTR wifiNetworkResume(void) {
  networkPaused=false;
  wifi_station_get_config(&stconf); wifi_softap_get_config(&apconf);
  os_timer_disarm(&resetTimer); os_timer_setfn(&resetTimer,resetTimerCb,NULL);
  os_timer_arm(&resetTimer,RESET_TIMEOUT,0);recoveryScheduled=true;
}

// Kick off connection to some network
void ICACHE_FLASH_ATTR connectToNetwork(char *ssid, char *pass) {
  os_strncpy((char*)stconf.ssid, ssid, 32);
  os_strncpy((char*)stconf.password, pass, 64);
  DBG("Wifi try to connect to AP %s\n", ssid);

  // Schedule disconnect/connect
  os_timer_disarm(&reassTimer);
  os_timer_setfn(&reassTimer, reassTimerCb, NULL);
  os_timer_arm(&reassTimer, 1000, 0); // 1 second for the response of this request to make it
}

// configure Wifi, specifically DHCP vs static IP address based on flash config
void ICACHE_FLASH_ATTR configWifiIP() {
  if (flashConfig.staticip == 0) {
    // let's DHCP!
    wifi_station_set_hostname(flashConfig.hostname);
    if (wifi_station_dhcpc_status() == DHCP_STARTED)
      wifi_station_dhcpc_stop();
    wifi_station_dhcpc_start();
    DBG("Wifi uses DHCP, hostname=%s\n", flashConfig.hostname);
  } else {
    // no DHCP, we got static network config!
    wifi_station_dhcpc_stop();
    struct ip_info ipi;
    ipi.ip.addr = flashConfig.staticip;
    ipi.netmask.addr = flashConfig.netmask;
    ipi.gw.addr = flashConfig.gateway;
    wifi_set_ip_info(0, &ipi);
    if (flashConfig.network_dns1) {
      ip_addr_t dns={.addr=flashConfig.network_dns1}; espconn_dns_setserver(0,&dns);
      dns.addr=flashConfig.network_dns2; espconn_dns_setserver(1,&dns);
    }
    DBG("Wifi uses static IP %d.%d.%d.%d\n", IP2STR(&ipi.ip.addr));
  }
}

void ICACHE_FLASH_ATTR wifiInit() {

    // Check the wifi opmode
    int x = wifi_get_opmode() & 0x3;

    // If STA is enabled switch to STA+AP to allow for recovery, it will then switch to STA-only
    // once it gets an IP address
    if (x == 1) wifi_set_opmode(3);

    // Call both STATION and SOFTAP default config
    wifi_station_get_config_default(&stconf);
    wifi_softap_get_config_default(&apconf);

    DBG("Wi-Fi init, mode=%d\n",x);

    if(wifi_get_opmode()&1)wifiRecoveryBegin();
    configWifiIP();
    apNetworkInit();

    // The default sleep mode should be modem_sleep, but we set it here explicitly for good
    // measure. We can't use light_sleep because that powers off everthing and we would loose
    // all connections.
    wifi_set_sleep_type(MODEM_SLEEP_T);

    wifi_set_event_handler_cb(wifiHandleEventCb);
    // check on the wifi in a few seconds to see whether we need to switch mode
    os_timer_disarm(&resetTimer);
    os_timer_setfn(&resetTimer, resetTimerCb, NULL);
    os_timer_arm(&resetTimer, RESET_TIMEOUT, 0);recoveryScheduled=true;
}

// Access functions for cgiWifiAps : query the number of entries in the table
int ICACHE_FLASH_ATTR wifiGetApCount() {
  if (cgiWifiAps.scanInProgress)
    return 0;
  return cgiWifiAps.noAps;
}

// Access functions for cgiWifiAps : returns the name of a network, i is the index into the array, return stored in memory pointed to by ptr.
ICACHE_FLASH_ATTR void wifiGetApName(int i, char *ptr) {
  if (i < 0)
    return;
  if (i >= cgiWifiAps.noAps)
    return;

  if (ptr != 0)
    strncpy(ptr, cgiWifiAps.apData[i]->ssid, 32);

  DBG("AP %s\n", cgiWifiAps.apData[i]->ssid);
}

// Access functions for cgiWifiAps : returns the signal strength of network (i is index into array). Return current network strength for negative i.
ICACHE_FLASH_ATTR int wifiSignalStrength(int i) {
  sint8 rssi;

  if (i < 0 || i == 255)
    rssi = wifi_station_get_rssi();	// Current network's signal strength
  else if (i >= cgiWifiAps.noAps)
    rssi = 0;				// FIX ME
  else
    rssi = cgiWifiAps.apData[i]->rssi;	// Signal strength of any known network

  return rssi;
}
