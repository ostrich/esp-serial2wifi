#!/usr/bin/env python3
"""Rename invariants and actual HTTP response identity; no device access."""
import _paths
import ast,json,re,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'.build-tests/branding';OUT.mkdir(parents=True,exist_ok=True)
for p in [*ROOT.glob('web/app/*.js'),ROOT/'web/app/index.html',ROOT/'web/custom-pages/ui.js']:
 if p.name=='xterm.js':continue
 s=p.read_text().replace('X-Serial2WiFi-Request','')
 s=re.sub(r'https://github.com/jeelabs/esp-link[^\s\'\"]*','',s)
 assert not re.search(r'esp[- ]link|ESP<span>LINK',s,re.I),p
recovery=next(ast.literal_eval(n.value) for n in ast.parse((ROOT/'tools/build/build_ui.py').read_text()).body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='RECOVERY' for t in n.targets))
assert 'ESP Serial2WiFi recovery' in recovery and 'X-Serial2WiFi-Request' in recovery
assert 'ESP-Link' not in recovery.replace('X-Serial2WiFi-Request','')
assert 'esp-serial2wifi v' in (ROOT/'web/app/updates.js').read_text()
assert 'esp-serial2wifi-assets'==json.loads((ROOT/'package.json').read_text())['name']==json.loads((ROOT/'package-lock.json').read_text())['packages']['']['name']
icon=(ROOT/'web/custom-pages/favicon.ico').read_bytes();assert struct.unpack('<HHH',icon[:6])==(0,1,2)
assert {(icon[6+16*i],icon[7+16*i]) for i in range(2)}=={(16,16),(32,32)}
# Verify product request guard and explicit on-flash format discriminators.
for name,token in [('src/core/api_request.c','X-Serial2WiFi-Request'),('tools/flash_ota.py','ELSDK306'),('tools/lib/ui_package.py','ELUPD001'),('src/core/ui_storage.c','ELUI001')]:assert token in (ROOT/name).read_text(),name
source=(ROOT/'src/httpd/httpd.c').read_text()
mime=source[source.index('typedef struct {\n  const char *ext;'):source.index('// debug string to identify connection')]
response=source[source.index('void ICACHE_FLASH_ATTR httpdStartResponse('):source.index('//Use this as a cgi function')]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ICACHE_FLASH_ATTR
#define os_strcmp strcmp
#define os_sprintf sprintf
typedef struct {int code;} Priv;
typedef struct {Priv *priv;} HttpdConnData;
static char output[2048];
static int httpdSend(HttpdConnData *c,const char *s,int n){(void)c;assert(strlen(output)+n<sizeof output);strncat(output,s,n);return 1;}
'''+mime+response+r'''
int main(void){Priv p;HttpdConnData c={&p};
 httpdStartResponse(&c,200);assert(strstr(output,"Server: esp-serial2wifi\r\n"));assert(p.code==200);
 output[0]=0;httpdRedirect(&c,"/index.html");assert(strstr(output,"Server: esp-serial2wifi\r\n"));assert(strstr(output,"Location: /index.html\r\n"));assert(p.code==302);
 assert(!strcmp(httpdGetMimetype("/favicon.ico"),"image/x-icon"));
 puts("PASS native HTTP response/redirect product headers and ICO MIME type");}
'''
(OUT/'headers.c').write_text(code)
subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(OUT/'headers.c'),'-o',str(OUT/'headers')],check=True)
subprocess.run([str(OUT/'headers')],check=True)
print('PASS active branding, recovery page, package identity, icon format and storage format identities')
