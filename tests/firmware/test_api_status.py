#!/usr/bin/env python3
"""Compile real JSON/status handlers with deterministic SDK observations."""

import _paths
from source_text import SourceText
from pathlib import Path
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".build-tests/api"
BUILD.mkdir(parents=True, exist_ok=True)

http = SourceText((ROOT / "src/httpd/httpd.c").read_text())
http_headers = http[
    http.index("void ICACHE_FLASH_ATTR httpdStartResponse(") : http.index(
        "//ToDo: sprintf->snprintf"
    )
]
cgi = SourceText((ROOT / "src/core/cgi.c").read_text())
cache_headers = cgi[
    cgi.index("void ICACHE_FLASH_ATTR noCacheHeaders(") : cgi.index(
        "void ICACHE_FLASH_ATTR jsonHeader("
    )
]
mqtt = SourceText((ROOT / "src/mqtt/mqtt.c").read_text())
connack = mqtt[
    mqtt.index("    case MQTT_MSG_TYPE_CONNACK:") : mqtt.index("    case MQTT_MSG_TYPE_SUBACK:")
]

prefix = r"""
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include "api.c"
#include "api_request.c"
int httpdGetHeader(HttpdConnData *c,char *name,char *out,int n){(void)c;(void)name;if(n)out[0]=0;return 0;}
#include "uptime.h"
FlashConfig flashConfig;
uint8_t wifiState;
#ifdef MQTT
MQTT_Client mqttClient;
void MQTT_Reconnect(MQTT_Client *c){c->connState=TCP_RECONNECT_REQ;}
#endif
char *firmwareVersion="esp-serial2wifi test";
static struct rst_info reset={6};
static uint8_t recoveryState;
static uint8_t mode=2,station=STATION_IDLE,listeners=3,slot,clients=2;
static bool dhcpFailed;
static bool ota,overrun;static unsigned queries,disconnected;
static struct station_config station_config;
static struct softap_config ap_config;
static UartRxStats rx;
static SerbridgeInputStats tx;
struct HttpdPriv {int code;};
static char response[2601];static size_t response_length;static int fail_after=-1;
int httpdSend(HttpdConnData *c,const char *data,int length){
 (void)c;if(length<0)length=strlen(data);
 if(fail_after==0)return 0;
 if(fail_after>0)fail_after--;
 if(response_length+(size_t)length>2600){overrun=true;return 0;}
 memcpy(response+response_length,data,length);response_length+=length;response[response_length]=0;return 1;
}
int espconn_disconnect(struct espconn *c){(void)c;disconnected++;return 0;}
bool wifiRecoveryDhcpFailed(void){return dhcpFailed;}
uint8_t wifiRecoveryStatus(void){return recoveryState;}
uint8_t wifi_get_opmode(void){queries++;return mode;}
uint8_t wifi_get_channel(void){return 1;}
uint8_t wifi_station_get_connect_status(void){return station;}
uint8_t wifi_softap_get_station_num(void){return clients;}
bool wifi_get_ip_info(uint8_t interface,struct ip_info *value){
 const uint8_t address[4]={192,168,4,interface==SOFTAP_IF?1:2};
 if(interface==STATION_IF && station!=STATION_GOT_IP)return false;
 memcpy(&value->ip.addr,address,4);return true;
}
bool wifi_station_get_config(struct station_config *v){*v=station_config;return true;}
bool wifi_softap_get_config(struct softap_config *v){*v=ap_config;return true;}
bool wifi_get_macaddr(uint8_t interface,uint8_t *value){(void)interface;memset(value,0x42,6);return true;}
int8_t wifi_station_get_rssi(void){return -57;}
uint8_t system_upgrade_userbin_check(void){return slot;}
uint32_t system_get_free_heap_size(void){return UINT32_MAX;}
struct rst_info *system_get_rst_info(void){return &reset;}
const char *system_get_sdk_version(void){return "3.0.6-test";}
void statusUptime(uint32_t *seconds,uint16_t *millis){*seconds=UINT32_MAX;*millis=999;}
uint8_t serbridge_listener_mask(void){return listeners;}
uint8_t serbridge_client_count(void){return 4;}
void uart_get_rx_stats(UartRxStats *v){*v=rx;}
void serbridge_get_input_stats(SerbridgeInputStats *v){*v=tx;}
static uint32_t webLost;
uint32_t consoleBootId(void){return 42;}
uint32_t consoleStreamTotalLost(void){return webLost;}
bool firmwareOTASupported(void){return ota;}
"""

suffix = r"""
static struct espconn socket;
static struct HttpdPriv priv;
static HttpdConnData connection={.conn=&socket,.requestType=HTTPD_METHOD_GET,.priv=&priv};
static void clear(void){response_length=0;response[0]=0;overrun=false;fail_after=-1;}
static void dump(const char *name){
 assert(!overrun);const char *body=strstr(response,"\r\n\r\n");assert(body);
 printf("%s\t%d\t%s\n",name,priv.code,body+4);
}
int main(void){
 strcpy(flashConfig.hostname,"host<&\"\\雪");strcpy(flashConfig.sys_descr,"A\nB\tC\"\\雪");
 strcpy(flashConfig.mqtt_password,"SECRET-MQTT");
 flashConfig.baud_rate=115200;flashConfig.data_bits=EIGHT_BITS;flashConfig.stop_bits=ONE_STOP_BIT;
 strcpy((char*)ap_config.ssid,"AP\"\\雪");ap_config.ssid_len=strlen((char*)ap_config.ssid);
 strcpy((char*)ap_config.password,"SECRET-AP");strcpy((char*)station_config.password,"SECRET-STA");
 strcpy((char*)station_config.ssid,"STA&+%雪");
 clear();apiStatus(&connection);dump("ap");
 clear();apiCapabilities(&connection);dump("capabilities");
 flashConfig.stop_bits=ONE_HALF_STOP_BIT;flashConfig.parity=ODD_BITS;
 clear();apiStatus(&connection);dump("half-stop");flashConfig.stop_bits=ONE_STOP_BIT;flashConfig.parity=NONE_BITS;
 mode=1;station=STATION_WRONG_PASSWORD;clear();apiStatus(&connection);dump("wrong-password");
 station=STATION_IDLE;recoveryState=STATION_NO_AP_FOUND;clear();apiStatus(&connection);dump("cached-recovery");recoveryState=0;
 station=STATION_CONNECTING;wifiState=wifiIsConnected;clear();apiStatus(&connection);dump("dhcp");
 dhcpFailed=true;clear();apiStatus(&connection);dump("dhcp-timeout");
 station=STATION_GOT_IP;clear();apiStatus(&connection);dump("station");dhcpFailed=false;
 mode=3;clear();apiStatus(&connection);dump("dual");
 mode=0;clear();apiStatus(&connection);dump("disabled");
 mode=2;tx.net_drops=1;clear();apiStatus(&connection);dump("loss");tx.net_drops=0;
 rx.framing_errors=2;tx.alloc_errors=3;clear();apiStatus(&connection);dump("events-only");rx.framing_errors=tx.alloc_errors=0;
 webLost=17;clear();apiStatus(&connection);dump("web-loss");webLost=0;
 rx.ring_drops=1;rx.framing_drops=2;rx.framing_errors=3;rx.fifo_overruns=4;
 tx.net_drops=5;tx.rejected=6;tx.alloc_errors=7;tx.net_alloc_errors=8;tx.net_send_errors=9;tx.hold_errors=10;webLost=11;
 clear();apiStatus(&connection);dump("each-counter");memset(&rx,0,sizeof rx);memset(&tx,0,sizeof tx);webLost=0;
 listeners=2;clear();apiStatus(&connection);dump("listener-failed");
 clear();apiCapabilities(&connection);dump("programmer-only");listeners=3;
 reset.reason=255;slot=9;clear();apiStatus(&connection);dump("unknown-reset-slot");reset.reason=6;slot=0;
#ifdef MQTT
 flashConfig.mqtt_enable=1;mqttClient.connState=MQTT_CONNECTED;
 clear();apiStatus(&connection);dump("mqtt-tcp-only");
 uint8_t ack[4]={0x20,2,0,5};mqttClient.in_buffer=ack;observeConnack(&mqttClient,4);
 clear();apiStatus(&connection);dump("mqtt-rejected");
 mqttClient.connState=MQTT_CONNECTED;ack[3]=0;observeConnack(&mqttClient,4);clear();apiStatus(&connection);dump("mqtt-accepted");
 uint8_t short_ack[2]={0x20,0};mqttClient.in_buffer=short_ack;
 observeConnack(&mqttClient,2);assert(mqttClient.connack_code==255);mqttClient.in_buffer=ack;
 ack[3]=6;observeConnack(&mqttClient,4);assert(mqttClient.connack_code==255);
#endif
 memset(flashConfig.hostname,31,sizeof(flashConfig.hostname));memset(flashConfig.sys_descr,31,sizeof(flashConfig.sys_descr));
 memset(ap_config.ssid,31,32);ap_config.ssid_len=32;rx.bytes_received=UINT32_MAX;
 memset(&rx,255,sizeof rx);memset(&tx,255,sizeof tx);webLost=UINT32_MAX;
 clear();apiStatus(&connection);dump("maximum");assert(priv.code==200);
 char huge_version[97];memset(huge_version,31,96);huge_version[96]=0;
 firmwareVersion=huge_version;clear();apiStatus(&connection);dump("oversized");assert(priv.code==503);
 firmwareVersion="esp-serial2wifi test";
 clear();connection.requestType=HTTPD_METHOD_POST;unsigned old_queries=queries;apiStatus(&connection);dump("method");assert(queries==old_queries);
 connection.requestType=HTTPD_METHOD_GET;connection.conn=NULL;clear();apiStatus(&connection);assert(!response_length&&queries==old_queries);connection.conn=&socket;
 clear();fail_after=8;apiStatus(&connection);assert(disconnected>0);
 // Streaming encoder: exact byte count, invalid UTF-8 replacement, and a
 // fraction outside the clock invariant must not corrupt the small buffer.
 clear();ApiJson json;apiJsonInit(&json,emit,&connection);
 const char invalid[]={ (char)0xc0,(char)0xaf,(char)0xed,(char)0xa0,(char)0x80,0 };
 apiJsonStringN(&json,invalid,5);printf("invalid-utf8\t200\t%s\n",response);
 clear();apiJsonInit(&json,emit,&connection);apiJsonMillis(&json,1,1000);assert(!strcmp(response,"null"));
 UptimeClock clock={0};uint32_t now=0;
 for(unsigned i=0;i<3000;i++){now+=3000000;uptimeUpdate(&clock,now);}
 assert(clock.seconds==9000&&clock.micros==0);
 clock.last=UINT32_MAX-500;clock.micros=999000;uptimeUpdate(&clock,600);
 assert(clock.seconds==9001&&clock.micros==101);
 return 0;
}
"""

for enabled in (False, True):
    path = BUILD / ("full.c" if enabled else "minimal.c")
    observer = (
        "#ifdef MQTT\nstatic void observeConnack(MQTT_Client *client,uint16_t msg_len){switch(MQTT_MSG_TYPE_CONNACK){\n"
        + connack
        + "default:break;}}\n#endif\n"
    )
    path.write_text(prefix + http_headers + cache_headers + observer + suffix)
    binary = path.with_suffix("")
    # config.h qualifies scalar return values; modern GCC diagnoses it.
    command = [
        "gcc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-parameter",
        "-Wno-ignored-qualifiers",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-I" + str(ROOT / "tests/support/api-stubs"),
        "-I" + str(ROOT / "src/core"),
        "-I" + str(ROOT / "src/httpd"),
        "-I" + str(ROOT / "src/serial"),
        "-I" + str(ROOT / "include"),
        "-I" + str(ROOT / "src/mqtt"),
        "-I" + str(ROOT / "build"),
        str(path),
        str(ROOT / "src/core/api_json.c"),
        "-o",
        str(binary),
    ]
    if enabled:
        command[1:1] = ["-DMQTT", "-DSYSLOG", "-DWEBSERVER", "-DREST", "-DSOCKET"]
    subprocess.run(command, check=True)
    output = subprocess.check_output(
        [str(binary)], text=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1")
    )
    assert "SECRET-" not in output
    cases = {}
    for line in output.splitlines():
        name, code, body = line.split("\t", 2)
        cases[name] = (int(code), json.loads(body))
    ap = cases["ap"][1]
    assert ap["hostname"] == 'host<&"\\雪'
    assert ap["description"] == 'A\nB\tC"\\雪'
    assert ap["wifi"]["ssid"] == 'AP"\\雪' and ap["wifi"]["ip"] == "192.168.4.1"
    assert ap["wifi"]["state"] == "ready" and ap["wifi"]["rssi_dbm"] is None
    assert ap["system"]["uptime_ms"] == 4294967295999
    assert ap["serial"]["target_connected"] is None and ap["serial"]["state"] == "ready"
    assert cases["half-stop"][1]["serial"]["configured_format"] == "8O1.5"
    assert cases["half-stop"][1]["wifi"]["mac"] == cases["ap"][1]["wifi"]["mac"]
    assert cases["wrong-password"][1]["wifi"]["reason"] == "wrong_password"
    assert cases["dhcp"][1]["wifi"]["reason"] == "dhcp_pending"
    assert cases["dhcp-timeout"][1]["wifi"]["reason"] == "dhcp_timeout"
    assert cases["dhcp-timeout"][1]["wifi"]["state"] == "failed"
    assert cases["station"][1]["wifi"]["reason"] is None
    assert cases["station"][1]["wifi"]["rssi_dbm"] == -57
    assert cases["dual"][1]["wifi"]["ap_ip"] == "192.168.4.1"
    assert cases["disabled"][1]["wifi"]["state"] == "disabled"
    assert cases["loss"][1]["serial"]["state"] == "ready"
    assert (
        cases["loss"][1]["serial"]["loss_detected"]
        and cases["loss"][1]["serial"]["errors"]["tcp_output_bytes"] == 1
    )
    assert not cases["events-only"][1]["serial"]["loss_detected"]
    assert cases["events-only"][1]["serial"]["errors"]["uart_frames"] == 2
    assert cases["each-counter"][1]["serial"]["errors"] == dict(
        zip(
            (
                "uart_ring_bytes",
                "uart_frame_bytes",
                "uart_frames",
                "uart_overruns",
                "tcp_output_bytes",
                "tcp_rejections",
                "tcp_input_alloc",
                "tcp_output_alloc",
                "tcp_send",
                "tcp_hold",
                "web_output_bytes",
            ),
            range(1, 12),
        )
    )
    assert cases["events-only"][1]["serial"]["errors"]["tcp_input_alloc"] == 3
    assert (
        cases["web-loss"][1]["serial"]["loss_detected"]
        and cases["web-loss"][1]["serial"]["errors"]["web_output_bytes"] == 17
    )
    assert cases["listener-failed"][1]["serial"]["state"] == "failed"
    assert cases["programmer-only"][1]["serial_tcp_ports"] == [2323]
    assert cases["unknown-reset-slot"][1]["firmware"]["slot"] is None
    assert cases["unknown-reset-slot"][1]["system"]["reset_reason"] == "unknown"
    assert cases["method"][0] == 405
    assert len(cases["maximum"][1]["hostname"]) == 32
    assert cases["invalid-utf8"][1] == "\ufffd" * 5
    assert cases["capabilities"][1]["features"]["mqtt"] is enabled
    assert cases["capabilities"][1]["features"]["custom_pages"] is enabled
    assert cases["capabilities"][1]["features"]["ota"] is False
    if enabled:
        assert cases["mqtt-tcp-only"][1]["mqtt"]["state"] == "connecting"
        assert cases["mqtt-rejected"][1]["mqtt"]["state"] == "failed"
        assert cases["mqtt-accepted"][1]["mqtt"]["state"] == "connected"
    else:
        assert "mqtt" not in ap
    (BUILD / ("full-results.json" if enabled else "minimal-results.json")).write_text(
        json.dumps(cases, indent=2) + "\n"
    )
print(
    "PASS real status/capabilities: redaction, escaped UTF-8/control strings, bounds, methods, feature guards, truthful states and clock wrap"
)
