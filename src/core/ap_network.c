#include <esp8266.h>
#include "ap_network.h"
#include "network_config.h"
#include "config.h"
#define AP_DEFAULT 0x0104a8c0U
#define AP_MASK 0x00ffffffU
static ETSTimer addressTimer;
static uint8_t attempts;
uint32_t ICACHE_FLASH_ATTR apNetworkAddress(void) {
  return networkAPValid(flashConfig.ap_address)?flashConfig.ap_address:AP_DEFAULT;
}
// SDK DHCP pools are constrained to the AP's /24. Twenty leases cover the
// maximum four associated stations and leave room for clients changing MACs.
bool ICACHE_FLASH_ATTR apNetworkConfigure(uint32_t address,bool persist) {
  struct ip_info old,check,desired={.ip={.addr=address},.netmask={.addr=AP_MASK},.gw={.addr=address}};
  struct dhcps_lease previous={0},lease={.enable=true};
  if(!networkAPValid(address)||!(wifi_get_opmode()&2)||
      !wifi_get_ip_info(SOFTAP_IF,&old))return false;
  bool running=wifi_softap_dhcps_status()==DHCP_STARTED;
  // The pinned SDK getter writes only the endpoints, not enable. With DHCP
  // stopped it returns false for its default (non-explicit) lease pool.
  if(wifi_softap_get_dhcps_lease(&previous))previous.enable=true;
  else if(running)return false;
  unsigned host=address>>24,start=host<=127?host+1:1;
  lease.start_ip.addr=(address&AP_MASK)|(start<<24);
  lease.end_ip.addr=(address&AP_MASK)|((start+19)<<24);
  uint32_t saved=flashConfig.ap_address,next=address==AP_DEFAULT?0:address;
  if(old.ip.addr!=address||old.netmask.addr!=AP_MASK||old.gw.addr!=address||!running) {
    if(running&&!wifi_softap_dhcps_stop())return false;
    if(!wifi_set_ip_info(SOFTAP_IF,&desired)||!wifi_softap_set_dhcps_lease(&lease)||
        !wifi_softap_dhcps_start()||!wifi_get_ip_info(SOFTAP_IF,&check)||
        check.ip.addr!=address||check.netmask.addr!=AP_MASK||check.gw.addr!=address||
        wifi_softap_dhcps_status()!=DHCP_STARTED)goto rollback;
  }
  if(persist&&saved!=next) {
    flashConfig.ap_address=next;
    if(!configSave()){flashConfig.ap_address=saved;goto rollback;}
  }
  return true;
rollback:
  // Keep the previous network usable even when saving the new setting fails.
  if(wifi_softap_dhcps_status()==DHCP_STARTED)wifi_softap_dhcps_stop();
  wifi_set_ip_info(SOFTAP_IF,&old);wifi_softap_set_dhcps_lease(&previous);
  if(running)wifi_softap_dhcps_start();
  return false;
}
static void ICACHE_FLASH_ATTR restoreAddress(void *unused) {
  (void)unused;
  if(apNetworkConfigure(apNetworkAddress(),false))return;
  if(++attempts<3)os_timer_arm(&addressTimer,500,0);
  else apNetworkConfigure(AP_DEFAULT,false);
}
void ICACHE_FLASH_ATTR apNetworkInit(void) {
  if(!flashConfig.ap_address)return; // Preserve the SDK default on existing devices.
  attempts=0;os_timer_disarm(&addressTimer);os_timer_setfn(&addressTimer,restoreAddress,NULL);
  restoreAddress(NULL); // Set the address before clients lease an IP when SDK is ready.
}
