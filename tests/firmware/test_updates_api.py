#!/usr/bin/env python3
"""Real update API guards and bounded inventory on an independently built ESPFS image."""

import _paths
import json, os, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/updates"
OUT.mkdir(parents=True, exist_ok=True)
source = r"""
#include <assert.h>
#include <esp8266.h>
#define os_strncmp strncmp
#define SPI_FLASH_RESULT_OK 0
int spi_flash_read(uint32_t,void *,unsigned);
#include "operation.c"
#include "api_request.c"
#include "updates.c"
char *firmwareVersion="esp-serial2wifi test";
void (*programmingCB)(char *,short);
static int code,uploads,activations,clears;static unsigned clients;
static char output[3000],guard[8]="1";static size_t used;
static uint8_t memory[FLASH_PAGES_SIZE];
const char *firmwareOTAReason(void){return "OTA unvalidated";}
bool firmwareOTASupported(void){return false;}
uint8_t system_upgrade_userbin_check(void){return 0;}
const char *system_get_sdk_version(void){return "3.0.6";}
uint32_t flashPhysicalSize(void){return FLASH_LAYOUT_BYTES;}
int serbridgeInMCUFlashing(void){return 0;}
bool serialInputPending(void){return false;}
uint8_t serbridge_client_count(void){return clients;}
int cgiUploadFirmware(HttpdConnData *c){(void)c;uploads++;return HTTPD_CGI_DONE;}
int cgiRebootFirmware(HttpdConnData *c){(void)c;activations++;return HTTPD_CGI_DONE;}
bool flashRegionContains(FlashRegion r,uint32_t a,uint32_t n){assert(r==FLASH_REGION_PAGES);return n&&a>=FLASH_PAGES_START&&a-FLASH_PAGES_START<=sizeof memory&&n<=sizeof memory-(a-FLASH_PAGES_START);}
int spi_flash_read(uint32_t a,void *p,unsigned n){assert(!(a&3)&&!(n&3)&&flashRegionContains(FLASH_REGION_PAGES,a,n));memcpy(p,memory+a-FLASH_PAGES_START,n);return 0;}
bool flashRegionErase(FlashRegion r,uint32_t a){assert(flashRegionContains(r,a,4096));memset(memory+a-FLASH_PAGES_START,255,4096);clears++;return true;}
bool flashRegionWrite(FlashRegion r,uint32_t a,const void *p,uint32_t n){assert(flashRegionContains(r,a,n));memcpy(memory+a-FLASH_PAGES_START,p,n);return true;}
#ifdef WEBSERVER
static bool uploading;
unsigned webServerHTMLHeaderLength(void){return 320;}
bool webServerUploadBusy(void){return uploading;}
void WEB_Init(void){}
int cgiWebServerSetupUpload(HttpdConnData *c){(void)c;uploads++;return HTTPD_CGI_DONE;}
#endif
int httpdSend(HttpdConnData *c,const char *p,int n){(void)c;if(n<0)n=strlen(p);assert(used+n<sizeof output);memcpy(output+used,p,n);used+=n;output[used]=0;return 1;}
void noCacheHeaders(HttpdConnData *c,int n){(void)c;code=n;}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdGetHeader(HttpdConnData *c,char *key,char *p,int n){(void)c;snprintf(p,n,"%s",!strcmp(key,"Content-Type")?"application/x-www-form-urlencoded":guard);return strlen(p);}
int espconn_disconnect(struct espconn *c){(void)c;assert(0);return 0;}
static struct espconn socket;
static HttpdPostData post;
static HttpdConnData conn={.conn=&socket,.post=&post,.url="/api/v1/pages"};
static int request(int(*fn)(HttpdConnData *),const char *body){code=0;used=0;output[0]=0;conn.requestType=body?HTTPD_METHOD_POST:HTTPD_METHOD_GET;post.buff=(char *)body;post.len=post.buffLen=post.received=body?strlen(body):0;fn(&conn);return code;}
int main(void){
 assert(request(apiFirmwareInfo,NULL)==200);printf("firmware\t%s\n",output);
 strcpy(guard,"");request(apiFirmwareUpload,"binary");assert(code==415&&!uploads);strcpy(guard,"1");
 clients=1;request(apiFirmwareUpload,"binary");assert(code==409&&!uploads);clients=0;
 request(apiFirmwareUpload,"binary");assert(uploads==1);
 request(apiFirmwareActivate,"action=wrong");assert(code==422&&!activations);
 request(apiFirmwareActivate,"action=activate_firmware");assert(activations==1);
#ifdef WEBSERVER
 memset(memory,255,sizeof memory);assert(request(apiPagesInfo,NULL)==200);printf("invalid\t%s\n",output);
 assert(request(apiPagesClear,"action=remove_all_pages")==200&&clears==1);printf("empty\t%s\n",output);
 // Header + padded UTF-8/control filename + empty payload + terminator.
 EspFsHeader file={.magic=ESPFS_MAGIC,.nameLen=12,.fileLenComp=0,.fileLenDecomp=0};
 memcpy(memory,&file,sizeof file);memset(memory+16,0,12);memcpy(memory+16,"name\"\n.html",11);
 uint32_t end[4]={ESPFS_MAGIC,FLAG_LASTFILE,0,0};memcpy(memory+28,end,sizeof end);
 assert(request(apiPagesInfo,NULL)==200);printf("one\t%s\n",output);
 conn.url="/api/v1/pages/1";assert(request(apiPagesInfo,NULL)==200);printf("past-end\t%s\n",output);
 conn.url="/api/v1/pages/99999999999";assert(request(apiPagesInfo,NULL)==422);conn.url="/api/v1/pages";
 uploading=true;assert(request(apiPagesClear,"action=remove_all_pages")==409&&clears==1);uploading=false;
 file.fileLenComp=FLASH_PAGES_SIZE;memcpy(memory,&file,sizeof file);assert(request(apiPagesInfo,NULL)==200);printf("outside\t%s\n",output);
#else
 assert(request(apiPagesInfo,NULL)==404);assert(request(apiPagesClear,"action=remove_all_pages")==404&&!clears);
#endif
}
"""
(OUT / "test.c").write_text(source)
for full in [False, True]:
    binary = OUT / ("full" if full else "minimal")
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
            *(["-DWEBSERVER"] if full else []),
            *[
                "-I" + str(ROOT / p)
                for p in [
                    "tests/support/api-stubs",
                    "src/core",
                    "src/httpd",
                    "src/serial",
                    "include",
                    "build",
                    "src/web-server",
                    "src/cmd",
                    "src/espfs",
                ]
            ],
            str(OUT / "test.c"),
            str(ROOT / "src/core/api_json.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    raw = subprocess.check_output(
        [str(binary)], text=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"}
    )
    cases = {k: json.loads(v) for k, v in (line.split("\t", 1) for line in raw.splitlines())}
    assert (
        not cases["firmware"]["ota_supported"] and cases["firmware"]["reason"] == "OTA unvalidated"
    )
    if full:
        assert not cases["invalid"]["valid"] and not cases["outside"]["valid"]
        assert (
            cases["empty"]["valid"]
            and cases["empty"]["count"] == 0
            and cases["empty"]["used_bytes"] == 16
        )
        assert cases["one"]["valid"] and cases["one"]["files"] == [
            {"name": 'name"\n.html', "bytes": 0}
        ]
        assert cases["past-end"]["files"] == []
print(
    "PASS update API: truthful OTA info, guarded activation/upload, bounded escaped/empty-file inventory, clear and busy guards; full/minimal modules"
)
