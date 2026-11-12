#include <esp8266.h>
#include "network.h"
#include "ap_network.h"
#include "network_config.h"
#include "api_request.h"
#include "operation.h"
#include "config.h"
#include "cgiwifi.h"
#include "wifi_recovery.h"

/* One pending change, one bounded scan. No retained request pointers/secrets in receipts. */
enum { CHANGE_NONE, CHANGE_STATION, CHANGE_AP, CHANGE_IDENTITY };
enum { OP_IDLE, OP_ACCEPTED, OP_APPLYING, OP_APPLIED, OP_FAILED };
static struct {
  uint8_t kind,state; uint32_t id;
  const char *error;
  union { struct station_config station; struct softap_config ap; } config;
  uint32_t ip[5]; char hostname[32],mdns_name[32]; bool mdns;
} change;
static ETSTimer applyTimer;
static bool changePrepared;
static bool scanPending,scanFinishing;
static const char *scanOutcome;
static const char *scanState="idle";
static uint8_t scanMode;
static unsigned scanCount,scanTotal;
typedef struct { uint8_t ssid[32],length,bssid[6],channel,auth; int8_t rssi; } NetworkAP;
static NetworkAP *scanResults;
static ETSTimer scanTimer,scanStartTimer,scanFinishTimer,expireTimer;
static size_t ICACHE_FLASH_ATTR boundedLength(const char *text,size_t limit) {
  size_t n=0;while(n<limit&&text[n])n++;return n;
}

#define string(j,key,value,length) do {apiJsonLiteral(j,key);apiJsonString(j,value,length);} while(0)
static void ICACHE_FLASH_ATTR address(ApiJson *j,uint32_t ip) {
  char text[16];const uint8_t *b=(const uint8_t *)&ip;
  os_sprintf(text,"%u.%u.%u.%u",b[0],b[1],b[2],b[3]);apiJsonString(j,text,sizeof(text));
}
static void ICACHE_FLASH_ATTR mac(ApiJson *j,const uint8_t b[6]) {
  char text[18];os_sprintf(text,"%02x:%02x:%02x:%02x:%02x:%02x",b[0],b[1],b[2],b[3],b[4],b[5]);
  apiJsonString(j,text,sizeof(text));
}
static void ICACHE_FLASH_ATTR receipt(ApiJson *j,const void *unused) {
  (void)unused;
  const char *states[]={"idle","accepted","applying","applied","failed"};
  apiJsonLiteral(j,"{\"id\":");apiJsonUnsigned(j,change.id);
  string(j,",\"state\":",states[change.state],16);
  string(j,",\"error\":",change.error,40);apiJsonLiteral(j,"}");
}
typedef struct {struct station_config station;struct softap_config ap;bool haveStation,haveAP;} ReadConfig;
static void ICACHE_FLASH_ATTR formatConfig(ApiJson *j,const void *data) {
  const ReadConfig *c=data;
  apiJsonLiteral(j,"{\"schema\":1,\"operation\":");receipt(j,NULL);
  apiJsonLiteral(j,",\"mode\":");unsigned mode=wifi_get_opmode()&3;
  apiJsonString(j,mode==2?"ap":mode==3?"station_ap":mode==1?"station":"disabled",16);
  if(c->haveStation) {
    string(j,",\"station\":{\"ssid\":",(const char*)c->station.ssid,32);
    apiJsonLiteral(j,",\"password_configured\":");apiJsonBool(j,c->station.password[0]!=0);
    apiJsonLiteral(j,",\"bssid\":");if(c->station.bssid_set)mac(j,c->station.bssid);else apiJsonLiteral(j,"null");
    apiJsonLiteral(j,"}");
  }else apiJsonLiteral(j,",\"station\":null");
  if(c->haveAP) {
    apiJsonLiteral(j,",\"ap\":{\"ssid\":");apiJsonStringN(j,(const char*)c->ap.ssid,c->ap.ssid_len<=32?c->ap.ssid_len:32);
    apiJsonLiteral(j,",\"password_configured\":");apiJsonBool(j,c->ap.password[0]!=0);
    string(j,",\"security\":",c->ap.authmode==AUTH_OPEN?"open":c->ap.authmode==AUTH_WPA_PSK?"wpa":
      c->ap.authmode==AUTH_WPA2_PSK?"wpa2":c->ap.authmode==AUTH_WPA_WPA2_PSK?"mixed":"unsupported",16);
    apiJsonLiteral(j,",\"channel\":");apiJsonUnsigned(j,c->ap.channel);
    apiJsonLiteral(j,",\"hidden\":");apiJsonBool(j,c->ap.ssid_hidden);
    apiJsonLiteral(j,",\"address\":");address(j,apNetworkAddress());apiJsonLiteral(j,"}");
  }else apiJsonLiteral(j,",\"ap\":null");
  apiJsonLiteral(j,",\"ip\":{\"dhcp\":");apiJsonBool(j,flashConfig.staticip==0);
  apiJsonLiteral(j,",\"address\":");address(j,flashConfig.staticip);
  apiJsonLiteral(j,",\"mask\":");address(j,flashConfig.netmask);
  apiJsonLiteral(j,",\"gateway\":");address(j,flashConfig.gateway);
  apiJsonLiteral(j,",\"dns1\":");address(j,flashConfig.network_dns1);
  apiJsonLiteral(j,",\"dns2\":");address(j,flashConfig.network_dns2);
  string(j,"},\"identity\":{\"hostname\":",flashConfig.hostname,sizeof(flashConfig.hostname));
  apiJsonLiteral(j,",\"mdns\":");apiJsonBool(j,flashConfig.mdns_enable);
  string(j,",\"mdns_name\":",flashConfig.mdns_servername,sizeof(flashConfig.mdns_servername));
  apiJsonLiteral(j,"}}");
}
int ICACHE_FLASH_ATTR apiNetwork(HttpdConnData *conn) {
  ReadConfig data;
  if(!apiRead(conn))return HTTPD_CGI_DONE;
  os_memset(&data,0,sizeof(data));
  data.haveStation=wifi_station_get_config(&data.station);data.haveAP=wifi_softap_get_config(&data.ap);
  int result=apiResponse(conn,200,formatConfig,&data);
  os_memset(&data,0,sizeof(data));return result;
}
static bool ICACHE_FLASH_ATTR applyIP(void) {
  if(flashConfig.staticip==0) {
    wifi_station_set_hostname(flashConfig.hostname);
    if(wifi_station_dhcpc_status()==DHCP_STARTED&&!wifi_station_dhcpc_stop())return false;
    return wifi_station_dhcpc_start();
  }
  if(wifi_station_dhcpc_status()==DHCP_STARTED&&!wifi_station_dhcpc_stop())return false;
  struct ip_info info={.ip={.addr=flashConfig.staticip},.netmask={.addr=flashConfig.netmask},.gw={.addr=flashConfig.gateway}};
  if(!wifi_set_ip_info(STATION_IF,&info))return false;
  ip_addr_t dns={.addr=flashConfig.network_dns1};espconn_dns_setserver(0,&dns);
  dns.addr=flashConfig.network_dns2;espconn_dns_setserver(1,&dns);return true;
}
static void ICACHE_FLASH_ATTR finishChange(bool okay) {
  change.state=okay?OP_APPLIED:OP_FAILED;if(okay)change.error=NULL;
  os_memset(&change.config,0,sizeof(change.config));
  os_memset(change.hostname,0,sizeof(change.hostname));os_memset(change.mdns_name,0,sizeof(change.mdns_name));
  wifiNetworkResume();deviceOperationEnd(&change);
}
static void ICACHE_FLASH_ATTR apply(void *unused) {
  (void)unused;bool okay=false;change.state=OP_APPLYING;
  if(!changePrepared)wifiNetworkPause();
  if(change.kind==CHANGE_AP&&!changePrepared) {
    changePrepared=true;change.error="ap_enable_failed";
    if(!wifi_set_opmode_current(3)){finishChange(false);return;}
    change.error="station_disconnect_failed";
    if(!wifi_station_disconnect()){finishChange(false);return;}
    /* AP initialization is not complete when the mode setter returns. The SDK
     * rejects AP settings while its startup/busy flag is set. */
    os_timer_disarm(&applyTimer);os_timer_setfn(&applyTimer,apply,NULL);os_timer_arm(&applyTimer,500,0);return;
  }
  if(change.kind==CHANGE_STATION) {
    uint32_t old[]={flashConfig.staticip,flashConfig.netmask,flashConfig.gateway,flashConfig.network_dns1,flashConfig.network_dns2};
    flashConfig.staticip=change.ip[0];flashConfig.netmask=change.ip[1];flashConfig.gateway=change.ip[2];
    flashConfig.network_dns1=change.ip[3];flashConfig.network_dns2=change.ip[4];
    if(!configSave()) {
      flashConfig.staticip=old[0];flashConfig.netmask=old[1];flashConfig.gateway=old[2];
      flashConfig.network_dns1=old[3];flashConfig.network_dns2=old[4];change.error="config_save_failed";
    }else {
      change.error="sdk_change_failed";
      okay=wifi_set_opmode_current(3)&&wifi_station_disconnect()&&wifiRecoveryBegin()&&
        wifi_station_set_config(&change.config.station)&&applyIP()&&wifi_set_opmode(3)&&
        wifi_station_set_auto_connect(1)&&wifi_station_connect();
      if(okay) {
        struct station_config readback;os_memset(&readback,0,sizeof(readback));
        okay=wifi_station_get_config_default(&readback)&&
          !os_memcmp(readback.ssid,change.config.station.ssid,32)&&
          !os_memcmp(readback.password,change.config.station.password,64)&&
          readback.bssid_set==change.config.station.bssid_set&&
          (!readback.bssid_set||!os_memcmp(readback.bssid,change.config.station.bssid,6));
        os_memset(&readback,0,sizeof(readback));
      }
    }
  }else if(change.kind==CHANGE_AP) {
    change.error="ap_config_failed";okay=wifi_softap_set_config(&change.config.ap);
    if(okay){change.error="ap_mode_failed";okay=wifi_set_opmode(2);}
    if(okay) {
      change.error="ap_readback_failed";
      struct softap_config readback;os_memset(&readback,0,sizeof(readback));
      okay=wifi_softap_get_config_default(&readback)&&readback.ssid_len==change.config.ap.ssid_len&&
        !os_memcmp(readback.ssid,change.config.ap.ssid,readback.ssid_len)&&
        !os_memcmp(readback.password,change.config.ap.password,64)&&readback.authmode==change.config.ap.authmode&&
        readback.channel==change.config.ap.channel&&readback.ssid_hidden==change.config.ap.ssid_hidden;
      os_memset(&readback,0,sizeof(readback));
    }
    if(okay){change.error="ap_address_failed";okay=apNetworkConfigure(change.ip[0],true);}
  }else if(change.kind==CHANGE_IDENTITY) {
    char oldHost[32],oldName[32];uint8_t oldEnabled=flashConfig.mdns_enable;
    os_memcpy(oldHost,flashConfig.hostname,32);os_memcpy(oldName,flashConfig.mdns_servername,32);
    os_memcpy(flashConfig.hostname,change.hostname,32);os_memcpy(flashConfig.mdns_servername,change.mdns_name,32);
    flashConfig.mdns_enable=change.mdns;okay=configSave();change.error="config_save_failed";
    if(!okay) {
      os_memcpy(flashConfig.hostname,oldHost,32);os_memcpy(flashConfig.mdns_servername,oldName,32);
      flashConfig.mdns_enable=oldEnabled;
    }else {
      change.error="sdk_change_failed";okay=!(wifi_get_opmode()&1)||wifi_station_set_hostname(flashConfig.hostname);
      /* Reboot safely refreshes discovery's SDK-owned pointers. Do not claim immediate mDNS. */
    }
  }
  finishChange(okay);
}
static int ICACHE_FLASH_ATTR accepted(HttpdConnData *conn,unsigned kind) {
  if(!deviceOperationBegin(&change))return apiError(conn,409,"DEVICE_BUSY","Wait for the current device operation.");
  changePrepared=false;change.kind=kind;change.state=OP_ACCEPTED;change.error=NULL;
  change.id=os_random();if(!change.id)change.id=1;
  os_timer_disarm(&applyTimer);os_timer_setfn(&applyTimer,apply,NULL);os_timer_arm(&applyTimer,1000,0);
  return apiResponse(conn,202,receipt,NULL);
}
static bool ICACHE_FLASH_ATTR available(HttpdConnData *conn) {
  if(!conn->conn)return false;
  if(change.state==OP_ACCEPTED||change.state==OP_APPLYING||scanPending||deviceOperationBusy()) {
    apiError(conn,409,"NETWORK_BUSY","A network operation is still in progress. Check its result before retrying.");return false;
  }
  return true;
}
static bool ICACHE_FLASH_ATTR password(char out[64],const char *action,const char *value,bool protected,bool same) {
  if(!os_strcmp(action,"keep"))return same&&!*value&&(protected==(out[0]!=0));
  if(!os_strcmp(action,"clear")) {if(protected||*value)return false;os_memset(out,0,64);return true;}
  if(os_strcmp(action,"replace")||!networkPassword(value,protected)||!protected)return false;
  os_memset(out,0,64);os_memcpy(out,value,os_strlen(value));return true;
}
#define FIELD(name,value) {name,value,sizeof(value),false}
int ICACHE_FLASH_ATTR apiNetworkStation(HttpdConnData *conn) {
  char ssid[33],security[12],pw[65],pwAction[12],bssid[18],dhcp[6],ip[16],mask[16],gateway[16],dns1[16],dns2[16];
  ApiField fields[]={FIELD("ssid",ssid),FIELD("security",security),FIELD("password_action",pwAction),FIELD("password",pw),
    FIELD("bssid",bssid),FIELD("dhcp",dhcp),FIELD("address",ip),FIELD("mask",mask),FIELD("gateway",gateway),FIELD("dns1",dns1),FIELD("dns2",dns2)};
  if(!available(conn)||!apiForm(conn,fields,sizeof(fields)/sizeof(*fields)))return HTTPD_CGI_DONE;
  struct station_config config;os_memset(&config,0,sizeof(config));bool okay=wifi_station_get_config(&config);
  bool same=okay&&os_strlen(ssid)>0&&os_strlen(ssid)==boundedLength((char*)config.ssid,32)&&!os_memcmp(ssid,config.ssid,os_strlen(ssid));
  if(!*ssid||(!os_strcmp(security,"open")?false:os_strcmp(security,"protected")!=0)||
      !password((char*)config.password,pwAction,pw,os_strcmp(security,"open")!=0,same))okay=false;
  os_memset(pw,0,sizeof(pw));
  config.channel=0;config.threshold.rssi=-127;config.threshold.authmode=AUTH_OPEN;
  config.open_and_wep_mode_disable=os_strcmp(security,"open")!=0;config.all_channel_scan=true;
  config.bssid_set=*bssid!=0;if(config.bssid_set&&!networkMac(bssid,config.bssid))okay=false;
  uint32_t ips[5]={0};
  if(!os_strcmp(dhcp,"false")) {if(!networkStatic(ip,mask,gateway,dns1,dns2,ips))okay=false;}
  else if(os_strcmp(dhcp,"true"))okay=false;
  if(!okay){os_memset(&config,0,sizeof(config));return apiError(conn,422,"INVALID_NETWORK","Check SSID, credentials, BSSID and static IP settings.");}
  os_memset(config.ssid,0,32);os_memcpy(config.ssid,ssid,os_strlen(ssid));
  change.config.station=config;os_memset(&config,0,sizeof(config));os_memcpy(change.ip,ips,sizeof(ips));
  return accepted(conn,CHANGE_STATION);
}
int ICACHE_FLASH_ATTR apiNetworkAP(HttpdConnData *conn) {
  char ssid[33],security[16],pw[65],pwAction[12],channel[4],hidden[6],ip[16];
  ApiField fields[]={FIELD("ssid",ssid),FIELD("security",security),FIELD("password_action",pwAction),FIELD("password",pw),FIELD("channel",channel),FIELD("hidden",hidden),FIELD("address",ip)};
  if(!available(conn)||!apiFormFields(conn,fields,sizeof(fields)/sizeof(*fields),6))return HTTPD_CGI_DONE;
  struct softap_config config;os_memset(&config,0,sizeof(config));bool okay=wifi_softap_get_config(&config);
  unsigned auth=!os_strcmp(security,"open")?AUTH_OPEN:!os_strcmp(security,"wpa2")?AUTH_WPA2_PSK:
    !os_strcmp(security,"mixed")?AUTH_WPA_WPA2_PSK:!os_strcmp(security,"wpa")?AUTH_WPA_PSK:255;
  if(!*ssid||auth==255||!password((char*)config.password,pwAction,pw,auth!=AUTH_OPEN,okay)||
      (auth!=AUTH_OPEN&&boundedLength((char*)config.password,64)>63))okay=false;
  os_memset(pw,0,sizeof(pw));
  unsigned ch=0;for(char *p=channel;*p;p++){if(*p<'0'||*p>'9'){okay=false;break;}ch=ch*10+*p-'0';}
  if(ch<1||ch>13||(os_strcmp(hidden,"true")&&os_strcmp(hidden,"false")))okay=false;
  if(!okay){os_memset(&config,0,sizeof(config));return apiError(conn,422,"INVALID_AP","Check SSID, security, password and channel (1 through 13).");}
  os_memset(config.ssid,0,32);config.ssid_len=os_strlen(ssid);os_memcpy(config.ssid,ssid,config.ssid_len);
  config.authmode=auth;config.channel=ch;config.ssid_hidden=!os_strcmp(hidden,"true");
  change.ip[0]=apNetworkAddress();
  if(fields[6].present&&(!networkIP(ip,&change.ip[0])||!networkAPValid(change.ip[0]))) {
    os_memset(&config,0,sizeof(config));return apiError(conn,422,"INVALID_AP_ADDRESS","Use an IPv4 host address ending in 1 through 254.");
  }
  change.config.ap=config;os_memset(&config,0,sizeof(config));return accepted(conn,CHANGE_AP);
}
int ICACHE_FLASH_ATTR apiNetworkIdentity(HttpdConnData *conn) {
  char host[32],name[32],enabled[6];
  ApiField fields[]={FIELD("hostname",host),FIELD("mdns_name",name),FIELD("mdns",enabled)};
  if(!available(conn)||!apiForm(conn,fields,3))return HTTPD_CGI_DONE;
  if(!networkHostname(host)||!networkHostname(name)||(os_strcmp(enabled,"true")&&os_strcmp(enabled,"false")))
    return apiError(conn,422,"INVALID_HOSTNAME","Use 1–31 ASCII letters, digits and interior hyphens.");
  os_memset(change.hostname,0,32);os_memcpy(change.hostname,host,os_strlen(host));
  os_memset(change.mdns_name,0,32);os_memcpy(change.mdns_name,name,os_strlen(name));change.mdns=!os_strcmp(enabled,"true");
  return accepted(conn,CHANGE_IDENTITY);
}

static void ICACHE_FLASH_ATTR expire(void *unused) {
  (void)unused;if(scanResults)os_free(scanResults);scanResults=NULL;scanCount=0;
  if(!scanPending)scanState="expired";
}
static bool ICACHE_FLASH_ATTR restoreScanMode(void) {
  bool okay=scanMode!=2||wifi_set_opmode_current(2);
  wifiNetworkResume();return okay;
}
static void ICACHE_FLASH_ATTR scanFinish(void *unused) {
  (void)unused;
  if(!restoreScanMode())scanOutcome="failed";
  scanState=scanOutcome;scanPending=false;scanFinishing=false;deviceOperationEnd(&scanResults);
  os_timer_disarm(&expireTimer);os_timer_setfn(&expireTimer,expire,NULL);os_timer_arm(&expireTimer,60000,0);
}
static void ICACHE_FLASH_ATTR finishLater(const char *outcome) {
  scanOutcome=outcome;scanFinishing=true;
  os_timer_disarm(&scanFinishTimer);os_timer_setfn(&scanFinishTimer,scanFinish,NULL);os_timer_arm(&scanFinishTimer,200,0);
}
static void ICACHE_FLASH_ATTR scanTimeout(void *unused) {
  (void)unused;expire(NULL);scanState="failed";
  /* Cancellation and completion are asynchronous. Keep the STA interface alive
   * until its callback unwinds; stopping it inside the scan callback can leave
   * the SDK's queued channel builder with a freed station pointer. */
  wifi_station_disconnect();
}
static void ICACHE_FLASH_ATTR scanDone(void *arg,STATUS status) {
  bool timedOut=os_strcmp(scanState,"scanning")!=0;
  os_timer_disarm(&scanTimer);
  if(!scanPending||scanFinishing)return;
  if(timedOut){finishLater("failed");return;}
  scanCount=scanTotal=0;
  if(status==OK&&scanResults) {
    struct bss_info *entry=arg;
    /* Bounded traversal and strongest-first insertion, preserving duplicate SSIDs. */
    for(unsigned visited=0;entry&&visited<256;visited++,entry=entry->next.stqe_next) {
      scanTotal++;unsigned at=0;while(at<scanCount&&scanResults[at].rssi>=entry->rssi)at++;
      if(at>=16)continue;
      if(scanCount<16)scanCount++;
      for(unsigned i=scanCount-1;i>at;i--)scanResults[i]=scanResults[i-1];
      NetworkAP *out=scanResults+at;os_memset(out,0,sizeof(*out));
      out->length=entry->ssid_len<=32?entry->ssid_len:32;os_memcpy(out->ssid,entry->ssid,out->length);
      os_memcpy(out->bssid,entry->bssid,6);out->rssi=entry->rssi;out->channel=entry->channel;out->auth=entry->authmode;
    }
    finishLater("complete");
  }else finishLater("failed");
}
static void ICACHE_FLASH_ATTR formatScan(ApiJson *j,const void *data) {
  unsigned page=*(const unsigned*)data;
  string(j,"{\"schema\":1,\"state\":",scanState,16);
  apiJsonLiteral(j,",\"total\":");apiJsonUnsigned(j,scanTotal);
  apiJsonLiteral(j,",\"retained\":");apiJsonUnsigned(j,scanCount);
  apiJsonLiteral(j,",\"page\":");apiJsonUnsigned(j,page);
  apiJsonLiteral(j,",\"truncated\":");apiJsonBool(j,scanTotal>scanCount);
  apiJsonLiteral(j,",\"networks\":[");
  if(!os_strcmp(scanState,"complete")&&scanResults)for(unsigned i=page*4;i<scanCount&&i<(page+1)*4;i++) {
    if(i>page*4)apiJsonLiteral(j,",");NetworkAP *ap=scanResults+i;
    apiJsonLiteral(j,"{\"ssid\":");apiJsonStringN(j,(const char*)ap->ssid,ap->length);
    apiJsonLiteral(j,",\"bssid\":");mac(j,ap->bssid);
    apiJsonLiteral(j,",\"rssi_dbm\":");apiJsonSigned(j,ap->rssi);
    apiJsonLiteral(j,",\"channel\":");apiJsonUnsigned(j,ap->channel);
    string(j,",\"security\":",ap->auth==AUTH_OPEN?"open":ap->auth==AUTH_WEP?"wep":ap->auth==AUTH_WPA_PSK?"wpa":
      ap->auth==AUTH_WPA2_PSK?"wpa2":ap->auth==AUTH_WPA_WPA2_PSK?"mixed":"unsupported",16);
    apiJsonLiteral(j,"}");
  }
  apiJsonLiteral(j,"]}");
}
static void ICACHE_FLASH_ATTR scanStart(void *unused) {
  (void)unused;if(!scanPending||scanFinishing)return;
  struct scan_config config;os_memset(&config,0,sizeof(config));config.show_hidden=1;
  config.scan_type=WIFI_SCAN_TYPE_ACTIVE;config.scan_time.active.min=20;config.scan_time.active.max=120;
  os_timer_disarm(&scanTimer);os_timer_setfn(&scanTimer,scanTimeout,NULL);os_timer_arm(&scanTimer,15000,0);
  if(!wifi_station_scan(&config,scanDone)){os_timer_disarm(&scanTimer);finishLater("failed");}
}
int ICACHE_FLASH_ATTR apiNetworkScan(HttpdConnData *conn) {
  char action[8];ApiField fields[]={FIELD("action",action)};
  if(!available(conn)||!apiForm(conn,fields,1))return HTTPD_CGI_DONE;
  if(os_strcmp(action,"start"))return apiError(conn,422,"INVALID_SCAN","Use the start action.");
  if(!deviceOperationBegin(&scanResults))return apiError(conn,409,"DEVICE_BUSY","Wait for the current device operation.");
  os_timer_disarm(&expireTimer);expire(NULL);
  scanResults=os_zalloc(16*sizeof(*scanResults));
  if(!scanResults){deviceOperationEnd(&scanResults);return apiError(conn,503,"SCAN_MEMORY","There is not enough memory to scan. Try after serial traffic stops.");}
  scanMode=wifi_get_opmode()&3;wifiNetworkPause();
  bool okay=true;
  if(scanMode==2)okay=wifi_set_opmode_current(3)&&wifi_station_disconnect();
  scanCount=scanTotal=0;scanState="scanning";scanPending=true;scanFinishing=false;
  if(!okay) {
    scanPending=false;deviceOperationEnd(&scanResults);scanState="failed";restoreScanMode();expire(NULL);scanState="failed";
    return apiError(conn,503,"SCAN_FAILED","The radio could not start a scan. Retry after it is idle.");
  }
  /* Let mode-start/disconnect events settle before asking the SDK to scan. */
  os_timer_disarm(&scanStartTimer);os_timer_setfn(&scanStartTimer,scanStart,NULL);os_timer_arm(&scanStartTimer,200,0);
  unsigned page=0;return apiResponse(conn,202,formatScan,&page);
}
int ICACHE_FLASH_ATTR apiNetworkScanPage(HttpdConnData *conn) {
  if(!apiRead(conn))return HTTPD_CGI_DONE;
  size_t n=os_strlen(conn->url);char last=n?conn->url[n-1]:0;
  if(n<2||conn->url[n-2]!='/'||last<'0'||last>'3')return apiError(conn,404,"SCAN_PAGE","Unknown scan page.");
  unsigned page=last-'0';return apiResponse(conn,200,formatScan,&page);
}
