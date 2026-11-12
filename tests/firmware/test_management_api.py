#!/usr/bin/env python3
"""Exercise real management handlers: staged saves, secrets, conflicts and rollback."""

import _paths
import os, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/management"
OUT.mkdir(parents=True, exist_ok=True)
(OUT / "sntp.h").write_text(
    "void sntp_stop(void);bool sntp_set_timezone(int8_t);void sntp_setservername(unsigned,char *);void sntp_init(void);\n"
)
source = r"""
#include <assert.h>
#include <esp8266.h>
#define os_malloc malloc
#define os_strcpy strcpy
#define os_strchr strchr
#define GPIO_DIS_OUTPUT(pin) ((void)(pin))
void system_restart(void);
#include "operation.c"
#include "api_request.c"
#include "management.c"
FlashConfig flashConfig;
void (*programmingCB)(char *,short);
uint8_t wifiState;
static bool failSave,busy;
static unsigned writes;
static int code;
static size_t used;
static char output[3000];
#ifdef MQTT
MQTT_Client mqttClient;
void MQTT_Free(MQTT_Client *c){(void)c;}
void MQTT_Connect(MQTT_Client *c){(void)c;}
void mqtt_client_init(void){}
#ifdef SYSLOG
void syslog_init(char *host){(void)host;}
#endif
#endif
uint8_t wifi_get_opmode(void){return 2;}
int8_t wifi_station_get_rssi(void){return 31;}
uint32_t system_get_free_heap_size(void){return 12000;}
bool configSave(void){writes++;flashConfig.seq++;return !failSave;}
int serbridgeInMCUFlashing(void){return 0;}
bool serialInputPending(void){return busy;}
uint8_t serbridge_client_count(void){return 0;}
void serbridgeInitPins(void){}
void serledInit(void){}
void statusInit(void){}
void system_restart(void){}
void sntp_stop(void){}
bool sntp_set_timezone(int8_t n){(void)n;return true;}
void sntp_setservername(unsigned i,char *s){(void)i;(void)s;}
void sntp_init(void){}
void os_timer_disarm(ETSTimer *t){t->armed=false;}
void os_timer_setfn(ETSTimer *t,void (*cb)(void *),void *arg){t->callback=cb;t->arg=arg;}
void os_timer_arm(ETSTimer *t,unsigned ms,bool repeat){assert(!repeat);t->armed=true;t->ms=ms;}
int httpdSend(HttpdConnData *c,const char *p,int n){(void)c;if(n<0)n=strlen(p);assert(used+n<sizeof output);memcpy(output+used,p,n);used+=n;output[used]=0;return 1;}
void noCacheHeaders(HttpdConnData *c,int n){(void)c;code=n;}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdGetHeader(HttpdConnData *c,char *key,char *p,int n){(void)c;snprintf(p,n,"%s",!strcmp(key,"Content-Type")?"application/x-www-form-urlencoded":"1");return strlen(p);}
int espconn_disconnect(struct espconn *c){(void)c;assert(0);return 0;}
static struct espconn socket;
static HttpdPostData post;
static HttpdConnData conn={.conn=&socket,.post=&post};
static int request(const char *kind,const char *body){
 code=0;used=0;output[0]=0;conn.cgiArg=(void *)kind;conn.requestType=body?HTTPD_METHOD_POST:HTTPD_METHOD_GET;
 post.buff=(char *)body;post.len=post.buffLen=post.received=body?strlen(body):0;
 if(body)apiManagementSave(&conn);else if(!strcmp(kind,"integrations"))apiIntegrations(&conn);else apiSettings(&conn);return code;
}
int main(void){
 strcpy(flashConfig.hostname,"original");flashConfig.mdns_enable=true;strcpy(flashConfig.mdns_servername,"original-mdns");strcpy(flashConfig.mqtt_password,"SAVED-SECRET");
 assert(request("settings",NULL)==200&&!strstr(output,"SAVED-SECRET"));
 assert(request("integrations",NULL)==200&&!strstr(output,"SAVED-SECRET"));
 assert(request("description","description=Recovery%20console")==200&&writes==1&&!pending);
 assert(!strcmp(flashConfig.hostname,"original")&&!strcmp(flashConfig.sys_descr,"Recovery console")&&flashConfig.mdns_enable&&!strcmp(flashConfig.mdns_servername,"original-mdns"));
 FlashConfig saved=flashConfig;failSave=true;
 assert(request("description","description=bad")==503&&!memcmp(&flashConfig,&saved,sizeof saved));failSave=false;
 unsigned before=writes;
 assert(request("description","description=bad&hostname=stale")==422&&writes==before);
 assert(request("identity","hostname=stale&description=bad")==404&&writes==before);
 assert(request("discovery","mdns_enable=false&mdns_name=stale")==404&&writes==before);
 strcpy(flashConfig.hostname,"network-changed");strcpy(flashConfig.mdns_servername,"network-mdns");flashConfig.mdns_enable=false;
 assert(request("description","description=Only%20description")==200&&writes==before+1&&!pending);
 assert(!strcmp(flashConfig.hostname,"network-changed")&&!strcmp(flashConfig.mdns_servername,"network-mdns")&&!flashConfig.mdns_enable);
 before=writes;
 assert(request("clock","sntp_server=time.example&timezone_offset=14")==422&&writes==before);
 assert(request("clock","sntp_server=time.example&timezone_offset=-5")==200&&flashConfig.timezone_offset==-5);
 assert(deviceOperationBusy()&&request("description","description=busy")==409);apply(NULL);assert(!deviceOperationBusy());
#ifdef SYSLOG
 assert(request("syslog","syslog_host=logs.example:514&syslog_min_heap=8192&syslog_filter=9&syslog_show_tick=true&syslog_show_date=false")==422);
 assert(request("syslog","syslog_host=logs.example:514&syslog_min_heap=8192&syslog_filter=6&syslog_show_tick=true&syslog_show_date=false")==200);apply(NULL);
#endif
 before=writes;
 assert(request("pins","reset=1&isp=-1&conn=2&ser=-1&swap=false&rx_pullup=true")==422&&writes==before);
 assert(request("pins","reset=0&isp=0&conn=2&ser=-1&swap=false&rx_pullup=true")==422&&writes==before);
 busy=true;assert(request("slip","enabled=true")==409&&writes==before);busy=false;
 assert(request("pins","reset=0&isp=-1&conn=2&ser=-1&swap=false&rx_pullup=true")==200);
 assert(flashConfig.reset_pin==0&&flashConfig.rx_pullup&&pending&APPLY_PINS);apply(NULL);
#ifdef MQTT
 const char *keep="enabled=true&host=broker.local&port=1883&client_id=test&username=user&password=&password_action=keep&clean_session=true&timeout=5&keepalive=30";
 assert(request("mqtt",keep)==200&&!strcmp(flashConfig.mqtt_password,"SAVED-SECRET")&&!strstr(output,"SAVED-SECRET"));apply(NULL);
 saved=flashConfig;failSave=true;assert(request("mqtt",keep)==503&&!memcmp(&flashConfig,&saved,sizeof saved));failSave=false;
 assert(request("mqtt","enabled=true&host=broker.local&port=1883&client_id=test&username=user&password=NEW-SECRET&password_action=replace&clean_session=true&timeout=5&keepalive=30")==200);
 assert(!strcmp(flashConfig.mqtt_password,"NEW-SECRET")&&!strstr(output,"NEW-SECRET"));apply(NULL);
 assert(request("mqtt","enabled=false&host=&port=1883&client_id=test&username=&password=&password_action=clear&clean_session=true&timeout=5&keepalive=30")==200&&!flashConfig.mqtt_password[0]);
#else
 assert(strstr(output,"\"syslog\":null"));
 assert(request("mqtt","enabled=true")==404);
#endif
 puts("PASS management saves: redacted reads, sequence, rollback, secret actions, GPIO conflicts and busy guard");
}
"""
(OUT / "test.c").write_text(source)
for mqtt in [False, True]:
    binary = OUT / ("full" if mqtt else "minimal")
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-g",
            "-O1",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-misleading-indentation",
            "-Wno-ignored-qualifiers",
            *(["-DMQTT", "-DSYSLOG"] if mqtt else []),
            *[
                "-I" + str(p)
                for p in [
                    OUT,
                    *[
                        ROOT / p
                        for p in [
                            "tests/support/api-stubs",
                            "src/core",
                            "src/httpd",
                            "src/serial",
                            "include",
                            "src/mqtt",
                            "src/syslog",
                        ]
                    ],
                ]
            ],
            str(OUT / "test.c"),
            str(ROOT / "src/core/api_json.c"),
            str(ROOT / "src/core/network_config.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})
