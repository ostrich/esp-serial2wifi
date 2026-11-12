#!/usr/bin/env python3
"""Exercise actual network endpoints/parser/validators with deterministic SDK faults."""
import _paths
from pathlib import Path
import os, subprocess
ROOT=Path(__file__).resolve().parents[2]
BUILD=ROOT/'.build-tests/network';BUILD.mkdir(parents=True,exist_ok=True)
header=r'''
#include <assert.h>
#include <stdlib.h>
#include "operation.c"
#include "api_request.c"
#include "network_config.c"
#include "ap_network.c"
#include "network.c"
FlashConfig flashConfig;
static uint8_t mode=2,autoconnect=1;
static struct ip_info apIP;
static struct dhcps_lease apLease;
static bool dhcps=true;static unsigned ipCalls,failIPAt;
static bool ipOkay(void){return ++ipCalls!=failIPAt;}
static struct station_config sta;
static struct softap_config ap;
static int code,queries,writes,pauses,resumes,disconnects;
static bool failSave,failSDK,failReadback,failAlloc,radioStarting,inSDKCallback;
static scan_done_cb_t scanCallback;
static char output[2601];static size_t used;
static char contentType[80]="application/x-www-form-urlencoded;charset=UTF-8",guard[8]="1";
void *test_zalloc(size_t n){return failAlloc?NULL:calloc(1,n);}
int httpdSend(HttpdConnData *c,const char *v,int n){(void)c;if(n<0)n=strlen(v);assert(used+(size_t)n<=2600);memcpy(output+used,v,n);used+=n;output[used]=0;return 1;}
void noCacheHeaders(HttpdConnData *c,int value){(void)c;code=value;}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdGetHeader(HttpdConnData *c,char *key,char *out,int n){(void)c;snprintf(out,n,"%s",!strcmp(key,"Content-Type")?contentType:guard);return strlen(out);}
int espconn_disconnect(struct espconn *c){(void)c;disconnects++;return 0;}
uint8_t wifi_get_opmode(void){queries++;return mode;}
bool wifi_set_opmode(uint8_t v){writes++;if(failSDK)return false;mode=v;return true;}
bool wifi_set_opmode_current(uint8_t v){assert(!inSDKCallback);writes++;if(failSDK)return false;if(v!=mode)radioStarting=true;mode=v;return true;}
bool wifi_station_get_config(struct station_config *v){queries++;*v=sta;return true;}
bool wifi_station_get_config_default(struct station_config *v){*v=sta;return !failReadback;}
bool wifi_softap_get_config(struct softap_config *v){queries++;*v=ap;return true;}
bool wifi_softap_get_config_default(struct softap_config *v){*v=ap;return !failReadback;}
bool wifi_station_set_config(struct station_config *v){writes++;sta=*v;return !failSDK;}
bool wifi_softap_set_config(struct softap_config *v){writes++;if(radioStarting)return false;ap=*v;return !failSDK;}
bool wifi_station_set_hostname(char *v){(void)v;writes++;return !failSDK;}
bool wifi_station_disconnect(void){writes++;return !failSDK;}
bool wifiRecoveryBegin(void){return !failSDK;}
bool wifi_station_connect(void){writes++;return !failSDK;}
uint8_t wifi_station_get_auto_connect(void){return autoconnect;}
bool wifi_station_set_auto_connect(uint8_t value){autoconnect=value;return true;}
enum dhcp_status wifi_station_dhcpc_status(void){return DHCP_STARTED;}
bool wifi_station_dhcpc_stop(void){writes++;return !failSDK;}
bool wifi_station_dhcpc_start(void){writes++;return !failSDK;}
bool wifi_get_ip_info(uint8_t i,struct ip_info *v){(void)i;if(!ipOkay())return false;*v=apIP;return true;}
bool wifi_set_ip_info(uint8_t i,struct ip_info *v){writes++;if(failSDK||!ipOkay())return false;if(i==SOFTAP_IF)apIP=*v;return true;}
enum dhcp_status wifi_softap_dhcps_status(void){return dhcps?DHCP_STARTED:DHCP_STOPPED;}
bool wifi_softap_dhcps_stop(void){if(!ipOkay())return false;dhcps=false;return true;}
bool wifi_softap_dhcps_start(void){if(!ipOkay())return false;dhcps=true;return true;}
bool wifi_softap_get_dhcps_lease(struct dhcps_lease *v){if(!ipOkay()||(!dhcps&&!apLease.enable))return false;v->start_ip=apLease.start_ip;v->end_ip=apLease.end_ip;return true;}
bool wifi_softap_set_dhcps_lease(struct dhcps_lease *v){assert(!dhcps);if(!ipOkay())return false;apLease=*v;return true;}
void espconn_dns_setserver(uint8_t i,ip_addr_t *v){(void)i;(void)v;writes++;}
bool configSave(void){writes++;return !failSave;}
void wifiNetworkPause(void){pauses++;}
void wifiNetworkResume(void){resumes++;}
void os_timer_disarm(ETSTimer *t){t->armed=false;}
void os_timer_setfn(ETSTimer *t,void (*cb)(void *),void *arg){t->callback=cb;t->arg=arg;}
void os_timer_arm(ETSTimer *t,unsigned ms,bool repeat){(void)repeat;t->armed=true;t->ms=ms;}
uint32_t os_random(void){static uint32_t v=100;return ++v;}
bool wifi_station_scan(struct scan_config *c,scan_done_cb_t callback){assert(!radioStarting);assert(c->show_hidden&&c->scan_time.active.max<=120);scanCallback=callback;return !failSDK;}
static struct espconn socket;
static HttpdPostData post;
static HttpdConnData connection={.conn=&socket,.post=&post,.url="/api/v1/network/scan/0"};
static int request(int (*handler)(HttpdConnData *),const char *body){
 code=0;used=0;output[0]=0;connection.requestType=body?HTTPD_METHOD_POST:HTTPD_METHOD_GET;
 post.buff=(char*)body;post.len=post.buffLen=post.received=body?strlen(body):0;
 handler(&connection);return code;
}
static void fire(ETSTimer *timer){assert(timer->armed);timer->armed=false;if(timer->ms>=200)radioStarting=false;timer->callback(timer->arg);}
static void completeScan(void *arg,STATUS status){inSDKCallback=true;scanCallback(arg,status);inSDKCallback=false;}
static void applyAll(void){fire(&applyTimer);if(applyTimer.armed)fire(&applyTimer);}
static void setup(void){
 memset(&change,0,sizeof(change));memset(&sta,0,sizeof(sta));memset(&ap,0,sizeof(ap));memset(&flashConfig,0,sizeof(flashConfig));
 strcpy((char*)sta.ssid,"old");strcpy((char*)sta.password,"SECRET-STA");
 strcpy((char*)ap.ssid,"recovery");ap.ssid_len=8;strcpy((char*)ap.password,"SECRET-AP");ap.channel=1;ap.authmode=AUTH_WPA2_PSK;ap.max_connection=4;
 strcpy(flashConfig.hostname,"demo");strcpy(flashConfig.mdns_servername,"core");
 connection.conn=&socket;mode=2;apIP=(struct ip_info){.ip={.addr=0x0104a8c0},.netmask={.addr=0x00ffffff},.gw={.addr=0x0104a8c0}};
 apLease=(struct dhcps_lease){.enable=true,.start_ip={.addr=0x0204a8c0},.end_ip={.addr=0x6404a8c0}};
 dhcps=true;ipCalls=failIPAt=0;memset(&addressTimer,0,sizeof addressTimer);
 radioStarting=inSDKCallback=false;failSave=failSDK=failReadback=failAlloc=false;strcpy(guard,"1");connection.getArgs=NULL;
}
static const char *stationBody="ssid=new%26%2B%25%E9%9B%AA&security=protected&password_action=replace&password=correct+horse&bssid=02%3A11%3A22%3A33%3A44%3A55&dhcp=true&address=&mask=&gateway=&dns1=&dns2=";
static const char *apBody="ssid=recovery&security=wpa2&password_action=keep&password=&channel=6&hidden=false";
int main(void){
 uint32_t ip,ips[5];uint8_t macbytes[6];assert(sizeof(FlashConfig)<=1024);
 assert(networkIP("192.168.4.1",&ip)&&ip==0x0104a8c0);assert(networkIP("0.0.0.0",&ip));
 const char *badIPs[]={"256.1.2.3","1..2.3","1.2.3.4.5","1.2.3","-1.2.3.4","1.2.3.4x","1234.2.3.4",""};
 for(unsigned i=0;i<sizeof(badIPs)/sizeof(*badIPs);i++)assert(!networkIP(badIPs[i],&ip));
 assert(networkStatic("10.1.2.3","255.255.255.0","10.1.2.1","1.1.1.1","0.0.0.0",ips));
 assert(networkStatic("10.1.2.3","255.255.255.0","0.0.0.0","0.0.0.0","0.0.0.0",ips));
 assert(!networkStatic("10.1.2.0","255.255.255.0","10.1.2.1","1.1.1.1","0.0.0.0",ips));
 assert(!networkStatic("10.1.2.3","255.0.255.0","10.1.2.1","1.1.1.1","0.0.0.0",ips));
 assert(!networkStatic("10.1.2.3","255.255.255.0","10.1.3.1","1.1.1.1","0.0.0.0",ips));
 assert(!networkStatic("10.1.2.3","255.255.255.255","10.1.2.1","1.1.1.1","0.0.0.0",ips));
 assert(networkPassword("12345678",true)&&!networkPassword("short",true)&&networkPassword("",false));
 assert(!networkPassword("password",false)&&networkHostname("recovery-demo")&&!networkHostname("-bad")&&!networkHostname("bad.local"));
 assert(networkMac("02:11:22:33:44:55",macbytes)&&!networkMac("ff:ff:ff:ff:ff:ff",macbytes));
 setup();used=0;apiError(&connection,422,"QUOTE_TEST","quote\\\"\\\\line\\n雪");assert(code==422&&strstr(output,"\\\"")&&strstr(output,"\\n"));
 setup();assert(request(apiNetwork,NULL)==200);assert(!strstr(output,"SECRET"));assert(strstr(output,"password_configured"));
 setup();int before=writes;assert(request(apiNetworkAP,NULL)==405&&writes==before);
 strcpy(guard,"");assert(request(apiNetworkAP,apBody)==415&&writes==before);strcpy(guard,"1");
 connection.getArgs="password=oops";assert(request(apiNetworkAP,apBody)==405&&writes==before);connection.getArgs=NULL;
 const char *bad[]={"ssid=x","ssid=x&ssid=y","ssid=%00","ssid=%GG","ssid=x&unknown=y","ssid=x&", "ssid=%FF&security=wpa2&password_action=keep&password=&channel=999&hidden=false"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){assert(request(apiNetworkAP,bad[i])==422);assert(writes==before);}
 setup();assert(request(apiNetworkStation,stationBody)==202);uint32_t id=change.id;assert(change.state==OP_ACCEPTED&&applyTimer.ms==1000);
 assert(request(apiNetworkAP,apBody)==409);applyAll();assert(change.state==OP_APPLIED&&change.id==id&&mode==3);
 assert(!strcmp((char*)sta.ssid,"new&+%雪")&&!strcmp((char*)sta.password,"correct horse")&&sta.bssid_set);
 for(unsigned i=0;i<sizeof(change.config);i++)assert(((unsigned char*)&change.config)[i]==0);
 assert(request(apiNetwork,NULL)==200&&!strstr(output,"correct horse"));
 setup();failSave=true;flashConfig.staticip=123;assert(request(apiNetworkStation,stationBody)==202);applyAll();
 assert(change.state==OP_FAILED&&mode==2&&flashConfig.staticip==123&&!strcmp((char*)sta.ssid,"old"));
 setup();failSDK=true;assert(request(apiNetworkAP,apBody)==202);applyAll();assert(change.state==OP_FAILED);
 setup();failReadback=true;assert(request(apiNetworkAP,apBody)==202);applyAll();assert(change.state==OP_FAILED);
 setup();assert(request(apiNetworkAP,apBody)==202);applyAll();assert(change.state==OP_APPLIED&&mode==2&&ap.channel==6&&ap.max_connection==4);
 setup();assert(request(apiNetworkStation,"ssid=different&security=protected&password_action=keep&password=&bssid=&dhcp=true&address=&mask=&gateway=&dns1=&dns2=")==422);
 setup();assert(request(apiNetworkIdentity,"hostname=new-host&mdns=true&mdns_name=core")==202);applyAll();assert(change.state==OP_APPLIED&&!strcmp(flashConfig.hostname,"new-host"));
 setup();assert(request(apiNetworkScan,"action=start")==202&&mode==3&&scanPending&&autoconnect==1);
 struct bss_info entries[20];memset(entries,0,sizeof(entries));
 for(unsigned i=0;i<20;i++){entries[i].next.stqe_next=i==19?NULL:entries+i+1;memset(entries[i].ssid,'A'+i,32);entries[i].ssid_len=32;entries[i].rssi=-80+i;entries[i].bssid[0]=2;entries[i].bssid[5]=i;entries[i].channel=1;entries[i].authmode=AUTH_WPA2_PSK;}
 fire(&scanStartTimer);completeScan(entries,OK);assert(scanPending&&mode==3);fire(&scanFinishTimer);assert(!scanPending&&mode==2&&autoconnect==1&&scanCount==16&&scanTotal==20&&scanResults[0].rssi==-61);
 assert(request(apiNetworkScanPage,NULL)==200&&strstr(output,"\"truncated\":true"));assert(used<2304);
 fire(&expireTimer);assert(!scanResults&&scanCount==0);
 assert(request(apiNetworkScan,"action=start")==202);fire(&scanStartTimer);fire(&scanTimer);assert(!scanResults&&mode==3&&scanPending&&!strcmp(scanState,"failed"));
 assert(request(apiNetworkScan,"action=start")==409);assert(request(apiNetworkAP,apBody)==409);
 completeScan(entries,OK);fire(&scanFinishTimer);assert(!scanPending&&!strcmp(scanState,"failed")&&mode==2);
 setup();failAlloc=true;assert(request(apiNetworkScan,"action=start")==503&&!scanResults);
 setup();failSDK=true;assert(request(apiNetworkScan,"action=start")==503&&!scanPending&&!scanResults);
 connection.conn=NULL;before=writes;apiNetworkAP(&connection);assert(writes==before);
 // All AP host octets produce a lease pool in the same /24 excluding the ESP.
 for(unsigned host=1;host<255;host++){
  setup();uint32_t desired=0x00071e0a|(host<<24);assert(apNetworkConfigure(desired,true));
  assert(flashConfig.ap_address==desired&&apIP.ip.addr==desired&&dhcps);
  assert((apLease.start_ip.addr&0x00ffffff)==(desired&0x00ffffff));
  unsigned first=apLease.start_ip.addr>>24,last=apLease.end_ip.addr>>24;
  assert(first>0&&last<255&&last-first==19&&!(first<=host&&host<=last));
 }
 // Every one-shot SDK fault restores the original address/lease/service/config.
 for(unsigned fault=1;fault<=7;fault++){
  setup();struct ip_info original=apIP;struct dhcps_lease lease=apLease;failIPAt=fault;
  assert(!apNetworkConfigure(0x01071e0a,true));assert(!flashConfig.ap_address&&dhcps);
  assert(!memcmp(&apIP,&original,sizeof original)&&!memcmp(&apLease,&lease,sizeof lease));
 }
 setup();dhcps=false;apLease=(struct dhcps_lease){0};assert(apNetworkConfigure(0x01071e0a,true)&&dhcps&&apIP.ip.addr==0x01071e0a);
 setup();dhcps=false;failIPAt=5;assert(!apNetworkConfigure(0x01071e0a,true));assert(!dhcps&&apIP.ip.addr==0x0104a8c0);
 setup();failSave=true;assert(!apNetworkConfigure(0x01071e0a,true));assert(apIP.ip.addr==0x0104a8c0&&!flashConfig.ap_address&&dhcps);
 setup();assert(request(apiNetworkAP,"ssid=recovery&security=wpa2&password_action=keep&password=&channel=6&hidden=false&address=10.30.7.254")==202);applyAll();
 assert(change.state==OP_APPLIED&&apIP.ip.addr==0xfe071e0a&&flashConfig.ap_address==0xfe071e0a);
 assert(request(apiNetwork,NULL)==200&&strstr(output,"10.30.7.254"));
 assert(request(apiNetworkAP,"ssid=recovery&security=wpa2&password_action=keep&password=&channel=6&hidden=false&address=10.30.7.0")==422);
 assert(request(apiNetworkAP,apBody)==202);applyAll();assert(apIP.ip.addr==0xfe071e0a); // older clients preserve address
 // Startup keeps defaults untouched and restores custom IP after SDK initialization.
 setup();apNetworkInit();assert(!addressTimer.armed);flashConfig.ap_address=0x01071e0a;
 before=writes;failIPAt=1;apNetworkInit();assert(addressTimer.armed&&addressTimer.ms==500);failIPAt=0;fire(&addressTimer);
 assert(apIP.ip.addr==flashConfig.ap_address&&writes==before+1&&!addressTimer.armed);
 setup();flashConfig.ap_address=0x01071e0a;failSDK=true;apNetworkInit();fire(&addressTimer);assert(addressTimer.armed);failSDK=false;fire(&addressTimer);assert(apIP.ip.addr==flashConfig.ap_address&&!addressTimer.armed);
 setup();flashConfig.ap_address=0x01071e0a;failSDK=true;apNetworkInit();
 fire(&addressTimer);fire(&addressTimer);assert(!addressTimer.armed&&flashConfig.ap_address==0x01071e0a&&apIP.ip.addr==0x0104a8c0&&dhcps);
 setup();flashConfig.ap_address=0x01071e0a;apNetworkInit();assert(!addressTimer.armed&&apIP.ip.addr==0x01071e0a);
 setup();assert(apNetworkConfigure(0x01071e0a,true));assert(apNetworkConfigure(0x0104a8c0,true));assert(!flashConfig.ap_address&&apIP.ip.addr==0x0104a8c0);
 assert(pauses==resumes);puts("PASS actual network API: strict body/credential validation, no secret reads, delayed receipts, save/SDK/readback failures, bounded scans/expiry/late callbacks, persistent AP addresses, DHCP rollback and AP restoration");
}
'''
(BUILD/'test.c').write_text(header)
subprocess.run(['cc','-std=c11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',
 '-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-ignored-qualifiers','-I'+str(ROOT/'tests/support/api-stubs'),'-I'+str(ROOT/'src/core'),'-I'+str(ROOT/'src/httpd'),
 str(BUILD/'test.c'),str(ROOT/'src/core/api_json.c'),'-o',str(BUILD/'test')],check=True)
subprocess.run([str(BUILD/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
