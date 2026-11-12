#include <esp8266.h>
#include "wifi_recovery.h"
static ETSTimer channelTimer;
static uint8_t failed,observed,attempts;
static void ICACHE_FLASH_ATTR restoreChannel(void *unused) {
  (void)unused;
  if(!failed||!(wifi_get_opmode()&2)||wifi_station_get_connect_status()==STATION_GOT_IP)return;
  struct softap_config ap;
  if(wifi_softap_get_config(&ap)&&wifi_set_channel(ap.channel))return;
  if(++attempts<3)os_timer_arm(&channelTimer,500,0);
}
bool ICACHE_FLASH_ATTR wifiRecoveryBegin(void) {
  os_timer_disarm(&channelTimer);failed=observed=0;attempts=0;
  /* RAM-only SDK retry policy: a failed explicit attempt must not keep taking
   * the shared radio away from the recovery AP. Re-enable after a real IP. */
  return wifi_station_set_reconnect_policy(false);
}
void ICACHE_FLASH_ATTR wifiRecoveryConnected(void) {
  os_timer_disarm(&channelTimer);failed=observed=0;wifi_station_set_reconnect_policy(true);
}
void ICACHE_FLASH_ATTR wifiRecoveryObserve(uint8_t reason) {
  if(observed==WIFI_RECOVERY_DHCP_TIMEOUT)return; /* Cancellation must retain the cause. */
  switch(reason) {
    case REASON_NO_AP_FOUND: observed=STATION_NO_AP_FOUND;break;
    case REASON_AUTH_FAIL: observed=STATION_WRONG_PASSWORD;break;
    case REASON_ASSOC_FAIL: case REASON_HANDSHAKE_TIMEOUT: case REASON_BEACON_TIMEOUT:
      observed=STATION_CONNECT_FAIL;break;
    default: observed=0;return;
  }
  uint8_t state=wifi_station_get_connect_status();
  if(state==STATION_WRONG_PASSWORD||state==STATION_NO_AP_FOUND||state==STATION_CONNECT_FAIL)observed=state;
}
void ICACHE_FLASH_ATTR wifiRecoveryDhcpTimeout(void) {observed=WIFI_RECOVERY_DHCP_TIMEOUT;}
bool ICACHE_FLASH_ATTR wifiRecoveryDhcpFailed(void) {return observed==WIFI_RECOVERY_DHCP_TIMEOUT||failed==WIFI_RECOVERY_DHCP_TIMEOUT;}
void ICACHE_FLASH_ATTR wifiRecoveryAssociated(void) {observed=0;}
bool ICACHE_FLASH_ATTR wifiRecoveryPoll(uint8_t state) {
  if(failed)return true;
  if(state==STATION_IDLE||state==STATION_CONNECTING||observed==WIFI_RECOVERY_DHCP_TIMEOUT)state=observed;
  if(state!=STATION_WRONG_PASSWORD&&state!=STATION_NO_AP_FOUND&&state!=STATION_CONNECT_FAIL&&state!=WIFI_RECOVERY_DHCP_TIMEOUT)return false;
  failed=state;attempts=0;wifi_station_set_reconnect_policy(false);
  wifi_station_disconnect();
  /* Let cancellation unwind. Keep the station interface allocated; tearing it
   * down while SDK scan work is queued causes a null station dereference. */
  os_timer_disarm(&channelTimer);os_timer_setfn(&channelTimer,restoreChannel,NULL);os_timer_arm(&channelTimer,500,0);
  return true;
}
uint8_t ICACHE_FLASH_ATTR wifiRecoveryStatus(void) {return failed;}
