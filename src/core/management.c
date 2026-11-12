#include <esp8266.h>
#include "management.h"
#include "api_request.h"
#include "operation.h"
#include "config.h"
#include "cgiwifi.h"
#include "serbridge.h"
#include "serial_api.h"
#include "serled.h"
#include "status.h"
#include "sntp.h"
#ifdef MQTT
#include "mqtt_client.h"
#endif
#ifdef SYSLOG
#include "syslog.h"
#endif

enum { APPLY_MQTT=1,APPLY_PINS=2,APPLY_CLOCK=4,APPLY_RESTART=8,APPLY_SYSLOG=16 };
static ETSTimer applyTimer;
static uint8_t pending;
static uint32_t releasePins;
#define RAW(j,s) apiJsonLiteral(j,s)
#define TEXT(j,key,v) do {RAW(j,key);apiJsonString(j,v,sizeof(v));} while(0)
static bool ICACHE_FLASH_ATTR number(const char *p,int low,int high,int *out) {
  bool negative=*p=='-';if(negative)p++;
  if(!*p)return false;unsigned n=0;
  for(;*p;p++){if(*p<'0'||*p>'9'||n>1000000U)return false;n=n*10+*p-'0';}
  if(n>2147483647U)return false;int value=negative?-(int)n:(int)n;
  if(value<low||value>high)return false;*out=value;return true;
}
static bool ICACHE_FLASH_ATTR boolean(const char *p,uint8_t *out) {
  if(!os_strcmp(p,"true")){*out=1;return true;}if(!os_strcmp(p,"false")){*out=0;return true;}return false;
}
static bool ICACHE_FLASH_ATTR host(const char *p,bool empty) {
  if(!*p)return empty;
  for(;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='.'))return false;
  return true;
}
static bool ICACHE_FLASH_ATTR pinAllowed(int p){return p==-1||(p>=0&&p<=5)||(p>=12&&p<=15);}
static uint32_t ICACHE_FLASH_ATTR pinMask(const FlashConfig *c) {
  int pins[]={c->reset_pin,c->isp_pin,c->conn_led_pin,c->ser_led_pin};uint32_t mask=0;
  for(unsigned i=0;i<4;i++)if(pins[i]>=0&&pinAllowed(pins[i]))mask|=1U<<pins[i];return mask;
}
static bool ICACHE_FLASH_ATTR pinsValid(const FlashConfig *c) {
  int pins[]={c->reset_pin,c->isp_pin,c->conn_led_pin,c->ser_led_pin};uint32_t mask=c->swap_uart?((1U<<13)|(1U<<15)):((1U<<1)|(1U<<3));
  for(unsigned i=0;i<4;i++) {int p=pins[i];if(!pinAllowed(p))return false;if(p<0)continue;if(mask&(1U<<p))return false;mask|=1U<<p;}
  return true;
}
static bool ICACHE_FLASH_ATTR serialBusy(void){return deviceOperationBusy()||programmingCB||serbridgeInMCUFlashing()||serialInputPending()||serbridge_client_count();}
static void ICACHE_FLASH_ATTR apply(void *unused) {
  (void)unused;uint8_t effects=pending;pending=0;
  if(effects&APPLY_RESTART){system_restart();return;}
#ifdef MQTT
  if(effects&APPLY_MQTT) {
    if(flashConfig.mqtt_enable){mqtt_client_init();if(wifiState==wifiGotIP||wifi_get_opmode()==2)MQTT_Connect(&mqttClient);}
    else MQTT_Free(&mqttClient);
  }
#endif
  if(effects&APPLY_PINS) {
    for(unsigned pin=0;pin<16;pin++)if(releasePins&(1U<<pin))GPIO_DIS_OUTPUT(pin);
    releasePins=0;serbridgeInitPins();serledInit();statusInit();
  }
  if(effects&APPLY_CLOCK) {
    sntp_stop();
    if(flashConfig.sntp_server[0]&&sntp_set_timezone(flashConfig.timezone_offset)){sntp_setservername(0,flashConfig.sntp_server);sntp_init();}
  }
#ifdef SYSLOG
  if(effects&APPLY_SYSLOG) {
    syslog_init(flashConfig.syslog_host);
  }
#endif
  deviceOperationEnd(&applyTimer);
}
static void ICACHE_FLASH_ATTR schedule(unsigned effects){pending|=effects;os_timer_disarm(&applyTimer);os_timer_setfn(&applyTimer,apply,NULL);os_timer_arm(&applyTimer,500,0);}
static void ICACHE_FLASH_ATTR settings(ApiJson *j,const void *unused) {
  (void)unused;RAW(j,"{\"schema\":1,\"sequence\":");apiJsonUnsigned(j,flashConfig.seq);
  RAW(j,",\"restart_required\":");apiJsonBool(j,false);RAW(j,",\"pending_apply\":");apiJsonBool(j,pending!=0);
  TEXT(j,",\"identity\":{\"hostname\":",flashConfig.hostname);TEXT(j,",\"description\":",flashConfig.sys_descr);
  RAW(j,"},\"pins\":{\"reset\":");apiJsonSigned(j,flashConfig.reset_pin);RAW(j,",\"isp\":");apiJsonSigned(j,flashConfig.isp_pin);
  RAW(j,",\"conn\":");apiJsonSigned(j,flashConfig.conn_led_pin);RAW(j,",\"ser\":");apiJsonSigned(j,flashConfig.ser_led_pin);
  RAW(j,",\"swap\":");apiJsonBool(j,flashConfig.swap_uart);RAW(j,",\"rx_pullup\":");apiJsonBool(j,flashConfig.rx_pullup);
  TEXT(j,"},\"services\":{\"sntp_server\":",flashConfig.sntp_server);RAW(j,",\"timezone_offset\":");apiJsonSigned(j,flashConfig.timezone_offset);
  RAW(j,",\"mdns_enable\":");apiJsonBool(j,flashConfig.mdns_enable);TEXT(j,",\"mdns_name\":",flashConfig.mdns_servername);
  RAW(j,",\"syslog\":");
#ifdef SYSLOG
  TEXT(j,"{\"host\":",flashConfig.syslog_host);RAW(j,",\"min_heap\":");apiJsonUnsigned(j,flashConfig.syslog_minheap);
  RAW(j,",\"filter\":");apiJsonUnsigned(j,flashConfig.syslog_filter);RAW(j,",\"show_tick\":");apiJsonBool(j,flashConfig.syslog_showtick);
  RAW(j,",\"show_date\":");apiJsonBool(j,flashConfig.syslog_showdate);RAW(j,"}");
#else
  RAW(j,"null");
#endif
  RAW(j,"}}");
}
static void ICACHE_FLASH_ATTR integrations(ApiJson *j,const void *unused) {
  (void)unused;RAW(j,"{\"schema\":1,\"sequence\":");apiJsonUnsigned(j,flashConfig.seq);RAW(j,",\"pending_apply\":");apiJsonBool(j,pending!=0);
  RAW(j,",\"slip\":{\"enabled\":");apiJsonBool(j,flashConfig.slip_enable);RAW(j,"},\"mqtt\":");
#ifdef MQTT
  RAW(j,"{\"enabled\":");apiJsonBool(j,flashConfig.mqtt_enable);TEXT(j,",\"host\":",flashConfig.mqtt_host);
  RAW(j,",\"port\":");apiJsonUnsigned(j,flashConfig.mqtt_port);TEXT(j,",\"client_id\":",flashConfig.mqtt_clientid);TEXT(j,",\"username\":",flashConfig.mqtt_username);
  RAW(j,",\"password_configured\":");apiJsonBool(j,flashConfig.mqtt_password[0]!=0);RAW(j,",\"clean_session\":");apiJsonBool(j,flashConfig.mqtt_clean_session);
  RAW(j,",\"timeout\":");apiJsonUnsigned(j,flashConfig.mqtt_timeout);RAW(j,",\"keepalive\":");apiJsonUnsigned(j,flashConfig.mqtt_keepalive);
  RAW(j,",\"reporting\":{\"enabled\":");apiJsonBool(j,flashConfig.mqtt_status_enable);TEXT(j,",\"topic\":",flashConfig.mqtt_status_topic);
  RAW(j,",\"interval_seconds\":60,\"last_successful_publish_ms\":null,\"sample\":{\"rssi\":");int rssi=wifi_station_get_rssi();apiJsonSigned(j,rssi>0?0:rssi);
  RAW(j,",\"heap_free\":");apiJsonUnsigned(j,system_get_free_heap_size());RAW(j,"}}}");
#else
  RAW(j,"null");
#endif
  RAW(j,"}");
}
int ICACHE_FLASH_ATTR apiSettings(HttpdConnData *c){if(!apiRead(c))return HTTPD_CGI_DONE;return apiResponse(c,200,settings,NULL);}
int ICACHE_FLASH_ATTR apiIntegrations(HttpdConnData *c){if(!apiRead(c))return HTTPD_CGI_DONE;return apiResponse(c,200,integrations,NULL);}
static void ICACHE_FLASH_ATTR swapConfig(FlashConfig *other) {
  unsigned char *a=(unsigned char*)&flashConfig,*b=(unsigned char*)other;
  for(unsigned i=0;i<sizeof(FlashConfig);i++){unsigned char t=a[i];a[i]=b[i];b[i]=t;}
}
#define FIELD(key,v) do {fields[count++]=(ApiField){key,v,sizeof(v),false};} while(0)
int ICACHE_FLASH_ATTR apiManagementSave(HttpdConnData *c) {
  if(!c->conn)return HTTPD_CGI_DONE;
  if(c->requestType!=HTTPD_METHOD_POST||(c->getArgs&&*c->getArgs))return apiError(c,405,"METHOD_NOT_ALLOWED","Use POST with a settings body.");
  if(pending&APPLY_RESTART)return apiError(c,409,"RESTART_PENDING","Wait for the pending restart.");
  if(deviceOperationBusy())return apiError(c,409,"DEVICE_BUSY","Wait for the current device operation.");
  const char *kind=c->cgiArg;int result=HTTPD_CGI_DONE,value,effects=0;
  FlashConfig *next=os_malloc(sizeof(*next));if(!next)return apiError(c,503,"SETTINGS_MEMORY","Not enough memory to stage settings.");
  os_memcpy(next,&flashConfig,sizeof(*next));ApiField fields[12];unsigned count=0;
  union {struct {char enabled[6],host[64],port[6],client[48],user[70],password[70],action[9],clean[6],timeout[4],keepalive[6];} mqtt;
    struct {char enabled[6],topic[32];} reporting;struct {char enabled[6];} slip;
    char description[129];
    struct {char reset[5],isp[5],conn[5],ser[5],swap[6],pullup[6];} pins;
    struct {char server[32],zone[5],host[32],heap[6],filter[2],tick[6],date[6];} services;} input;
  if(!os_strcmp(kind,"mqtt")) {
#ifndef MQTT
    result=apiError(c,404,"FEATURE_UNAVAILABLE","MQTT is not compiled in this build.");goto done;
#else
    FIELD("enabled",input.mqtt.enabled);FIELD("host",input.mqtt.host);FIELD("port",input.mqtt.port);FIELD("client_id",input.mqtt.client);
    FIELD("username",input.mqtt.user);FIELD("password",input.mqtt.password);FIELD("password_action",input.mqtt.action);FIELD("clean_session",input.mqtt.clean);
    FIELD("timeout",input.mqtt.timeout);FIELD("keepalive",input.mqtt.keepalive);if(!apiForm(c,fields,count))goto done;
    if(!boolean(input.mqtt.enabled,&next->mqtt_enable)||!host(input.mqtt.host,!next->mqtt_enable)||
        (next->mqtt_enable&&!input.mqtt.client[0])||!number(input.mqtt.port,1,65535,&value))goto invalid;
    next->mqtt_port=value;if(!number(input.mqtt.timeout,1,255,&value))goto invalid;next->mqtt_timeout=value;
    if(!number(input.mqtt.keepalive,1,65535,&value)||!boolean(input.mqtt.clean,&next->mqtt_clean_session))goto invalid;next->mqtt_keepalive=value;
    os_strcpy(next->mqtt_host,input.mqtt.host);os_strcpy(next->mqtt_clientid,input.mqtt.client);os_strcpy(next->mqtt_username,input.mqtt.user);
    if(!os_strcmp(input.mqtt.action,"replace")){if(!input.mqtt.password[0])goto invalid;os_strcpy(next->mqtt_password,input.mqtt.password);}
    else if(!os_strcmp(input.mqtt.action,"clear")){if(input.mqtt.password[0])goto invalid;next->mqtt_password[0]=0;}
    else if(os_strcmp(input.mqtt.action,"keep")||input.mqtt.password[0])goto invalid;effects=APPLY_MQTT;
#endif
  }else if(!os_strcmp(kind,"reporting")) {
#ifndef MQTT
    result=apiError(c,404,"FEATURE_UNAVAILABLE","MQTT is not compiled in this build.");goto done;
#else
    FIELD("enabled",input.reporting.enabled);FIELD("topic",input.reporting.topic);if(!apiForm(c,fields,count))goto done;
    if(!boolean(input.reporting.enabled,&next->mqtt_status_enable)||(next->mqtt_status_enable&&!input.reporting.topic[0])||os_strchr(input.reporting.topic,'#')||os_strchr(input.reporting.topic,'+'))goto invalid;
    os_strcpy(next->mqtt_status_topic,input.reporting.topic);
#endif
  }else if(!os_strcmp(kind,"slip")) {
    FIELD("enabled",input.slip.enabled);if(!apiForm(c,fields,count))goto done;if(serialBusy()){result=apiError(c,409,"SERIAL_BUSY","Finish MCU programming or queued input first.");goto done;}
    if(!boolean(input.slip.enabled,&next->slip_enable))goto invalid;
  }else if(!os_strcmp(kind,"description")) {
    FIELD("description",input.description);if(!apiForm(c,fields,count))goto done;
    os_strcpy(next->sys_descr,input.description);
  }else if(!os_strcmp(kind,"pins")) {
    FIELD("reset",input.pins.reset);FIELD("isp",input.pins.isp);FIELD("conn",input.pins.conn);FIELD("ser",input.pins.ser);FIELD("swap",input.pins.swap);FIELD("rx_pullup",input.pins.pullup);
    if(!apiForm(c,fields,count))goto done;if(serialBusy()){result=apiError(c,409,"SERIAL_BUSY","Finish MCU programming or queued input first.");goto done;}
    if(!number(input.pins.reset,-1,15,&value))goto invalid;next->reset_pin=value;
    if(!number(input.pins.isp,-1,15,&value))goto invalid;next->isp_pin=value;
    if(!number(input.pins.conn,-1,15,&value))goto invalid;next->conn_led_pin=value;
    if(!number(input.pins.ser,-1,15,&value))goto invalid;next->ser_led_pin=value;
    if(!boolean(input.pins.swap,&next->swap_uart)||!boolean(input.pins.pullup,(uint8_t*)&next->rx_pullup)||!pinsValid(next))goto invalid;effects=APPLY_PINS;
  }else if(!os_strcmp(kind,"clock")) {
    FIELD("sntp_server",input.services.server);FIELD("timezone_offset",input.services.zone);if(!apiForm(c,fields,count))goto done;
    if(!host(input.services.server,true)||!number(input.services.zone,-11,13,&value))goto invalid;
    next->timezone_offset=value;os_strcpy(next->sntp_server,input.services.server);effects=APPLY_CLOCK;
  }else if(!os_strcmp(kind,"syslog")) {
#ifndef SYSLOG
    result=apiError(c,404,"FEATURE_UNAVAILABLE","Syslog is not compiled in this build.");goto done;
#else
    FIELD("syslog_host",input.services.host);FIELD("syslog_min_heap",input.services.heap);FIELD("syslog_filter",input.services.filter);FIELD("syslog_show_tick",input.services.tick);FIELD("syslog_show_date",input.services.date);
    if(!apiForm(c,fields,count))goto done;
    char hostname[32];os_strcpy(hostname,input.services.host);char *colon=os_strchr(hostname,':');
    if(colon){*colon++=0;if(!number(colon,1,65535,&value))goto invalid;}
    if(!host(hostname,true))goto invalid;
    os_strcpy(next->syslog_host,input.services.host);if(!number(input.services.heap,0,65535,&value))goto invalid;next->syslog_minheap=value;
    if(!number(input.services.filter,0,7,&value))goto invalid;next->syslog_filter=value;
    if(!boolean(input.services.tick,&next->syslog_showtick)||!boolean(input.services.date,&next->syslog_showdate))goto invalid;
    effects=APPLY_SYSLOG;
#endif
  }else {result=apiError(c,404,"UNKNOWN_SETTINGS","Unknown settings group.");goto done;}
  if(effects&&!deviceOperationBegin(&applyTimer)){result=apiError(c,409,"DEVICE_BUSY","Wait for the current device operation.");goto done;}
  {uint32_t oldPins=pinMask(&flashConfig);swapConfig(next);
    if(!configSave()){if(effects)deviceOperationEnd(&applyTimer);swapConfig(next);result=apiError(c,503,"SAVE_FAILED","Settings could not be saved; reload before retrying.");goto done;}
    if(effects&APPLY_PINS)releasePins|=oldPins;
    if(effects)schedule(effects);
    result=apiResponse(c,200,(!os_strcmp(kind,"mqtt")||!os_strcmp(kind,"reporting")||!os_strcmp(kind,"slip"))?integrations:settings,NULL);goto done;}
invalid:
  result=apiError(c,422,"INVALID_SETTINGS","Check required fields, ranges, secret actions and GPIO/UART conflicts.");
done:
  os_memset(&input,0,sizeof input);os_memset(next,0,sizeof(*next));os_free(next);return result;
}
static void ICACHE_FLASH_ATTR accepted(ApiJson *j,const void *unused){(void)unused;RAW(j,"{\"schema\":1,\"accepted\":true}");}
int ICACHE_FLASH_ATTR apiManagementRestart(HttpdConnData *c) {
  char action[16];ApiField field={"action",action,sizeof action,0};if(!apiForm(c,&field,1))return HTTPD_CGI_DONE;
  if(os_strcmp(action,"restart_esp"))return apiError(c,422,"INVALID_ACTION","Choose Restart ESP Serial2WiFi explicitly.");
  if(serialBusy())return apiError(c,409,"SERIAL_BUSY","Finish MCU programming or queued input before restarting.");
  if(!deviceOperationBegin(&applyTimer))return apiError(c,409,"DEVICE_BUSY","Wait for the current device operation.");
  schedule(APPLY_RESTART);return apiResponse(c,202,accepted,NULL);
}
