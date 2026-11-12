#!/usr/bin/env python3
"""Custom-page navigation must remain bounded JSON and survive OOM."""
import _paths
import json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'.build-tests/page-menu';OUT.mkdir(parents=True,exist_ok=True)
s=(ROOT/'src/web-server/web-server.c').read_text();body=s[s.index('char * webServerPages'):s.index('// initializer')]
cgi=(ROOT/'src/core/cgi.c').read_text();formatter=cgi[cgi.index('static void ICACHE_FLASH_ATTR pageMenu('):cgi.index('int ICACHE_FLASH_ATTR cgiMenu(')]
source=r'''
#include <assert.h>
#include <esp8266.h>
#define os_malloc allocate
#include "api_json.h"
#include "config.h"
#include "mqtt_client.h"
FlashConfig flashConfig;char *firmwareVersion="version\"quoted";
typedef struct {char name[256];unsigned index;} EspFsIterator;
static bool fail;static char longName[250];static void *userPageCtx;
static void *allocate(size_t n){return fail?NULL:malloc(n);}
static int espFsIsValid(void *ctx){(void)ctx;return 1;}
static void espFsIteratorInit(void *ctx,EspFsIterator *it){(void)ctx;it->index=0;}
static int espFsIteratorNext(EspFsIterator *it){if(it->index==12)return 0;strcpy(it->name,it->index++?longName:"folder/a\"\n.html");return 1;}
''' + body + formatter + r'''
static int printJSON(void *ctx,const char *data,size_t n){(void)ctx;fwrite(data,1,n,stdout);return 1;}
int main(void){
 memset(longName,'a',sizeof longName);strcpy(longName+240,".html");
 assert(!strcmp(WEB_UserPages(),""));WEB_BrowseFiles();assert(webServerPages&&strlen(webServerPages)<1024);
 printf("[0%s]\n",webServerPages);strcpy(flashConfig.hostname,"name\"quoted");ApiJson j;apiJsonInit(&j,printJSON,NULL);pageMenu(&j,NULL);assert(j.okay&&j.length<2304);puts("");char *old=webServerPages;fail=true;WEB_BrowseFiles();assert(webServerPages==old);free(webServerPages);
}
'''
(OUT/'test.c').write_text(source)
subprocess.run(['cc','-std=c11','-DWEBSERVER','-DMQTT','-g','-O1','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-ignored-qualifiers',*['-I'+str(ROOT/p) for p in ['tests/support/api-stubs','src/core','src/mqtt','include']],str(OUT/'test.c'),str(ROOT/'src/core/api_json.c'),'-o',str(OUT/'test')],check=True)
raw=subprocess.check_output([str(OUT/'test')],text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'});lines=raw.splitlines();values=json.loads(lines[0]);menu=json.loads(lines[1]);assert menu['version']=='version\"quoted' and menu['name']=='name\"quoted' and menu['menu'][-2:]==values[-2:]
assert values[1:3]==['a"\n','/user/folder/a"\n.html'] and 3<len(values)<25
print('PASS actual custom-page menu: bounded accumulated entries, escaped labels/paths and allocation-failure preservation')
