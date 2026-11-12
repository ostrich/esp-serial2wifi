#!/usr/bin/env python3
"""Real diagnostic snapshot: complete lines, bounded raw cursors and token masking."""

import _paths
from source_text import SourceText
import base64, json, os, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/log-api"
OUT.mkdir(parents=True, exist_ok=True)
log = SourceText((ROOT / "src/core/log.c").read_text())
snapshot = log[log.index("// A bounded snapshot") : log.index("static char *dbg_mode")]
modeCode = log[log.index("static char *dbg_mode") : log.index("int ICACHE_FLASH_ATTR\najaxLogDbg(")]
source = (
    r"""
#include <assert.h>
#include <esp8266.h>
#include "config.h"
#include "log.h"
#include "cgiwifi.h"
#include "operation.c"
#include "api_request.c"
#define BUF_MAX 1400
static char log_buf[BUF_MAX];static int log_rd,log_wr;
static uint32_t log_pos,log_boot=42;
FlashConfig flashConfig;static bool log_no_uart,failSave,busy;uint8_t wifiState=wifiGotIP;
void (*programmingCB)(char *,short);
bool configSave(void){return !failSave;}
int serbridgeInMCUFlashing(void){return busy;}
bool serialInputPending(void){return false;}
void log_uart(bool enable){log_no_uart=!enable;}
static char output[2600];static size_t used;static int code;
int httpdSend(HttpdConnData *c,const char *p,int n){(void)c;if(n<0)n=strlen(p);assert(used+n<sizeof output);memcpy(output+used,p,n);used+=n;output[used]=0;return 1;}
void noCacheHeaders(HttpdConnData *c,int n){(void)c;code=n;}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdGetHeader(HttpdConnData *c,char *key,char *p,int n){(void)c;snprintf(p,n,"%s",!strcmp(key,"Content-Type")?"application/x-www-form-urlencoded":"1");return strlen(p);}
int espconn_disconnect(struct espconn *c){(void)c;assert(0);return 0;}
"""
    + snapshot
    + modeCode
    + r"""
static void append(char c){log_buf[log_wr]=c;log_wr=(log_wr+1)%BUF_MAX;if(log_wr==log_rd){log_rd=(log_rd+1)%BUF_MAX;log_pos++;}}
static void writeText(const char *p){while(*p)append(*p++);}
static struct espconn socket;
static HttpdConnData conn={.conn=&socket,.requestType=HTTPD_METHOD_GET};
static void capture(const char *name){used=0;code=0;apiDiagnosticsLog(&conn);assert(code==200);printf("%s\t%s\n",name,output);}
int main(void){
 strcpy(flashConfig.mqtt_password,"SECRET");
 writeText("normal SECRET\r\npending SECRET");capture("masked");
 writeText(" done\r\n");capture("complete");
 for(unsigned i=0;i<200;i++)writeText("SECRET repeated\r\n");capture("wrapped");
 log_rd=log_wr=0;log_pos=UINT32_MAX-4;writeText("croppedSECRET\nvalid SECRET\n");capture("u32");
 conn.requestType=HTTPD_METHOD_POST;used=0;apiDiagnosticsLog(&conn);assert(code==405);
 HttpdPostData post={0};conn.post=&post;flashConfig.log_mode=LOG_MODE_OFF;log_no_uart=true;
 post.buff="mode=on1";post.len=post.buffLen=post.received=strlen(post.buff);
 failSave=true;used=0;apiDiagnosticsLogMode(&conn);assert(code==503&&flashConfig.log_mode==LOG_MODE_OFF&&log_no_uart);
 failSave=false;used=0;apiDiagnosticsLogMode(&conn);assert(code==200&&flashConfig.log_mode==LOG_MODE_ON1&&!log_no_uart&&strstr(output,"\"mode\":\"on1\""));
 busy=true;used=0;apiDiagnosticsLogMode(&conn);assert(code==409);
}
"""
)
(OUT / "test.c").write_text(source)
subprocess.run(
    [
        "cc",
        "-std=c11",
        "-g",
        "-O1",
        "-fsanitize=address,undefined",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-misleading-indentation",
        "-Wno-ignored-qualifiers",
        *["-I" + str(ROOT / p) for p in ["tests/support/api-stubs", "src/core", "src/httpd"]],
        str(OUT / "test.c"),
        str(ROOT / "src/core/api_json.c"),
        "-o",
        str(OUT / "test"),
    ],
    check=True,
)
raw = subprocess.check_output(
    [str(OUT / "test")], text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"}
)
packets = {k: json.loads(v) for k, v in (line.split("\t", 1) for line in raw.splitlines())}
for p in packets.values():
    data = base64.b64decode(p["data"])
    assert len(data) <= 1023 and len(data) == (p["end"] - p["start"]) % 2**32
    assert b"SECRET" not in data
    assert not data or data.endswith(b"\n")
assert base64.b64decode(packets["masked"]["data"]) == b"normal ******\r\n"
assert base64.b64decode(packets["u32"]["data"]) == b"valid ******\n"
print(
    "PASS real debug log snapshot: masking, cropped/incomplete lines, wraparound, byte cursors and read-only guard"
)
