#!/usr/bin/env python3
"""Public defaults, request guards and persisted configuration ABI."""

from source_text import SourceText
from pathlib import Path
import json, subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/defaults"
OUT.mkdir(parents=True, exist_ok=True)
(OUT / "esp8266.h").write_text(r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define AUTH_OPEN 0
#define SOFTAP_MODE 2
#define os_memset memset
#define os_snprintf snprintf
#define os_strlen strlen
struct softap_config {uint8_t ssid[32],password[64],ssid_len,channel,authmode,ssid_hidden,max_connection;uint16_t beacon_interval;};
static uint32_t system_get_chip_id(void){return 0x123456;}
static bool modeOkay=true,apOkay=true;static unsigned mode,apCalls;static struct softap_config saved;
static bool wifi_set_opmode(unsigned value){mode=value;return modeOkay;}
static bool wifi_softap_set_config(struct softap_config *config){saved=*config;apCalls++;return apOkay;}
""")
(OUT / "test.c").write_text(
    '#include <esp8266.h>\n#include "'
    + str(ROOT / "src/core/wifi_setup.c")
    + '"\n'
    + r"""
int main(void){
 assert(wifiSetupDefaults()&&mode==2&&apCalls==1);
 assert(!strcmp((char*)saved.ssid,"ESP-Serial2WiFi-123456")&&saved.ssid_len==strlen((char*)saved.ssid));
 assert(saved.authmode==AUTH_OPEN&&!saved.password[0]&&!saved.ssid_hidden);
 assert(saved.channel==1&&saved.max_connection==4&&saved.beacon_interval==100);
 modeOkay=false;assert(!wifiSetupDefaults()&&apCalls==1);
 modeOkay=true;apOkay=false;assert(!wifiSetupDefaults()&&apCalls==2);
 puts("PASS first boot: unique standalone setup AP, no shared credentials, bounded SDK failures");
}
"""
)
subprocess.run(
    [
        "cc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-I" + str(OUT),
        str(OUT / "test.c"),
        "-o",
        str(OUT / "test"),
    ],
    check=True,
)
subprocess.run([str(OUT / "test")], check=True)
source = SourceText((ROOT / "src/core/main.c").read_text())
assert (
    "restoreOk || wifiSetupDefaults()" in source
    and "!restoreOk && defaultsReady && !configSave()" in source
)
make = SourceText((ROOT / "Makefile").read_text())
for name in ("MCU_RESET_PIN", "MCU_ISP_PIN", "LED_CONN_PIN", "LED_SERIAL_PIN"):
    assert name + " ?= -1" in make
config = SourceText((ROOT / "src/core/config.c").read_text())
assert config.index(".log_mode = 1") >= 0
assert config.index('.sntp_server = "\\0"') >= 0
assert "CHIP_IN_HOSTNAME ?= 1" in make
print(
    "PASS saved settings bypass setup; default pins/debug UART/time/MQTT/SLIP do not assume local wiring/services"
)

OUT = ROOT / ".build-tests/project-boundary"
OUT.mkdir(parents=True, exist_ok=True)
source = SourceText((ROOT / "src/core/api_request.c").read_text())
body = source[
    source.index("bool ICACHE_FLASH_ATTR apiRead(") : source.index(
        "static int ICACHE_FLASH_ATTR hex("
    )
]
code = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define ICACHE_FLASH_ATTR
#define HTTPD_METHOD_GET 1
#define HTTPD_METHOD_POST 2
#define os_strcmp strcmp
#define apiError(c,code,name,message) (errorCode=code)
typedef struct {void *conn;int requestType;char *getArgs;} HttpdConnData;
static int errorCode;
static const char *incomingName,*incomingValue;
static int httpdGetHeader(HttpdConnData *c,const char *name,char *value,unsigned n){
 (void)c;assert(!strcmp(name,"X-Serial2WiFi-Request"));
 snprintf(value,n,"%s",incomingName&&!strcmp(incomingName,name)?incomingValue:"");return value[0]!=0;
}
"""
    + body
    + r"""
int main(void){
 HttpdConnData c={.conn=&c,.requestType=HTTPD_METHOD_POST};
 incomingName="X-Unrelated-Request";incomingValue="1";assert(!apiWrite(&c)&&errorCode==415);
 incomingName="X-Serial2WiFi-Request";assert(apiWrite(&c));
 incomingValue="true";assert(!apiWrite(&c)&&errorCode==415);
 incomingName=NULL;assert(!apiWrite(&c)&&errorCode==415);
 incomingName="X-Serial2WiFi-Request";incomingValue="1";
 c.getArgs="action=anything";assert(!apiWrite(&c)&&errorCode==405);c.getArgs=NULL;
 c.requestType=HTTPD_METHOD_GET;assert(!apiWrite(&c)&&errorCode==405);assert(apiRead(&c));
 c.conn=NULL;assert(!apiWrite(&c)&&!apiRead(&c));
 puts("PASS actual request guard accepts only the product header, POST and no query mutations");
}
"""
)
(OUT / "test.c").write_text(code)
subprocess.run(
    [
        "cc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        str(OUT / "test.c"),
        "-o",
        str(OUT / "test"),
    ],
    check=True,
)
subprocess.run([str(OUT / "test")], check=True)
layout = json.loads(SourceText((ROOT / "tests/support/config-layout.json").read_text()))
checks = (
    '#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n#include "'
    + str(ROOT / "src/core/config.h")
    + '"\n'
)
for key, value in layout.items():
    expression = "sizeof(FlashConfig)" if key == "size" else "offsetof(FlashConfig," + key + ")"
    checks += "typedef char layout_" + key + "[(" + expression + "==" + str(value) + ")?1:-1];\n"
(OUT / "layout.c").write_text(checks)
subprocess.run(
    ["cc", "-std=c99", "-c", str(OUT / "layout.c"), "-o", str(OUT / "layout.o")], check=True
)
print("PASS configuration record size and every live field offset match the persisted layout")
