#!/usr/bin/env python3
"""Current product/API boundary: no upstream migration, guards or admin routes."""
import _paths
from pathlib import Path
import subprocess,json

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'.build-tests/project-boundary';OUT.mkdir(parents=True,exist_ok=True)
source=(ROOT/'src/core/api_request.c').read_text()
body=source[source.index('bool ICACHE_FLASH_ATTR apiRead('):source.index('static int ICACHE_FLASH_ATTR hex(')]
code=r'''
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
'''+body+r'''
int main(void){
 HttpdConnData c={.conn=&c,.requestType=HTTPD_METHOD_POST};
 incomingName="X-ESP-Link-Request";incomingValue="1";assert(!apiWrite(&c)&&errorCode==415);
 incomingName="X-Serial2WiFi-Request";assert(apiWrite(&c));
 incomingValue="true";assert(!apiWrite(&c)&&errorCode==415);
 incomingName=NULL;assert(!apiWrite(&c)&&errorCode==415);
 incomingName="X-Serial2WiFi-Request";incomingValue="1";
 c.getArgs="action=anything";assert(!apiWrite(&c)&&errorCode==405);c.getArgs=NULL;
 c.requestType=HTTPD_METHOD_GET;assert(!apiWrite(&c)&&errorCode==405);assert(apiRead(&c));
 c.conn=NULL;assert(!apiWrite(&c)&&!apiRead(&c));
 puts("PASS actual request guard accepts only the product header, POST and no query mutations");
}
'''
(OUT/'test.c').write_text(code)
subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(OUT/'test.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True)
routes=(ROOT/'src/core/main.c').read_text()
for path in ('/system/info','/flash/next','/flash/upload','/flash/reboot','/wifi/info','/mqtt'):
 assert '"'+path+'"' not in routes,path
for directory in ('src/core','src/serial','src/cmd','src/httpd','src/espfs','include','web/app'):
 for p in (ROOT/directory).rglob('*'):
  if not p.is_file() or p.suffix not in ('.c','.h','.js','.html'):continue
  if directory=='ui' and 'vendor' in p.parts:continue
  s=p.read_text()
  for obsolete in ('ESP_LINK_','esp_link_version','espLinkCtx','X-ESP-Link-Request',"localStorage.getItem('esp-link-"):
   assert obsolete not in s,(p,obsolete)
assert not (ROOT/'docs/history').exists() and not (ROOT/'espmake.cmd').exists()
print('PASS source/API boundary: retired routes, identifiers, migration preferences and build recipes absent')

layout=json.loads((ROOT/'tests/config-layout.json').read_text())
checks='#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n#include "'+str(ROOT/'src/core/config.h')+'"\n'
for key,value in layout.items():
 expression='sizeof(FlashConfig)' if key=='size' else 'offsetof(FlashConfig,'+key+')'
 checks+='typedef char layout_'+key+'[('+expression+'=='+str(value)+')?1:-1];\n'
(OUT/'layout.c').write_text(checks)
subprocess.run(['cc','-std=c99','-c',str(OUT/'layout.c'),'-o',str(OUT/'layout.o')],check=True)
print('PASS configuration record size and every live field offset match the persisted layout')
