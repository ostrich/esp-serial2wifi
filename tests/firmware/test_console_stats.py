#!/usr/bin/env python3
"""Format actual stats CGI with maximum counters under ASan/UBSan."""
import _paths
import json
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'src/serial/console.c').read_text()
function = source[source.index('int ICACHE_FLASH_ATTR\najaxConsoleStats('):]
records = ''
for filename, name in [('uart.h', 'UartRxStats'), ('serbridge.h', 'SerbridgeInputStats')]:
    header = (root/'src/serial' / filename).read_text()
    records += re.search(r'typedef struct \{[^}]+\} ' + name + ';', header).group() + '\n'
prelude = r'''
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define HTTPD_CGI_DONE 1
#define os_sprintf sprintf
typedef struct {void *conn;} HttpdConnData;
static char response[1024];
static void jsonHeader(HttpdConnData *c,int status){assert(c->conn&&status==200);}
static void httpdSend(HttpdConnData *c,char *data,int len){assert(c->conn&&len==-1);assert(strlen(data)<sizeof response);strcpy(response,data);}
static unsigned system_get_time(void){return UINT32_MAX;}
static unsigned system_get_free_heap_size(void){return UINT32_MAX;}
static unsigned wifi_get_sleep_type(void){return UINT32_MAX;}
'''
stubs = r'''
static void uart_get_rx_stats(UartRxStats *s){memset(s,255,sizeof *s);}
static void serbridge_get_input_stats(SerbridgeInputStats *s){memset(s,255,sizeof *s);s->net_last_error=INT32_MIN;}
'''
tests = r'''
int main(void){
 HttpdConnData c={0};assert(ajaxConsoleStats(&c)==HTTPD_CGI_DONE&&!response[0]);
 c.conn=&c;assert(ajaxConsoleStats(&c)==HTTPD_CGI_DONE);puts(response);
}
'''
out = root / '.build-tests'
out.mkdir(exist_ok=True)
cfile = out / 'test-console-stats.c'
cfile.write_text(prelude + records + stubs + function + tests)
binary = out / 'test-console-stats'
subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wformat=2',
                '-fsanitize=address,undefined', str(cfile), '-o', str(binary)], check=True)
result = subprocess.check_output([str(binary)], text=True)
data = json.loads(result)
assert data['received'] == 0xffffffff and data['net_fail_rcv_nxt'] == 0xffffffff
assert data['net_last_error'] == -2147483648
assert data['net_queue_high_water'] == 0xffffffff
print(f'PASS actual stats CGI: disconnected request and maximum counters ({len(result.strip())} JSON bytes)')
