#!/usr/bin/env python3
"""Actual TCP input admission: early backpressure, bounded copying and faults."""

import _paths
from source_text import SourceText
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
source = SourceText((root / "src/serial/serbridge.c").read_text())
start = source.index("static bool ICACHE_FLASH_ATTR\nholdUartInput(")
end = source.index("static void ICACHE_FLASH_ATTR\nbeginProgramming(", start)
macro = re.search(r"^#define MAX_RXBUFFER .+$", source, re.M).group()
code = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define ICACHE_FLASH_ATTR
#define ESPCONN_OK 0
#define os_memmove memmove
#define os_memcpy memcpy
struct espconn {int unused;};
typedef struct {struct espconn *conn;char *rxbuffer;uint16_t rxbufferlen,rxbufferpos;bool rx_held,tx_failed;} serbridgeConnData;
static struct {unsigned received,max_callback,rejected,hold_errors,hold_calls,alloc_errors,high_water;} input_stats;
static unsigned holds,unholds,closes,allocs,fail_hold,fail_unhold,fail_alloc;
static int espconn_recv_hold(struct espconn *c){assert(c);holds++;return fail_hold?-1:0;}
static int espconn_recv_unhold(struct espconn *c){assert(c);unholds++;return fail_unhold?-1:0;}
static int espconn_disconnect(struct espconn *c){assert(c);closes++;return 0;}
static void *os_malloc(size_t n){assert(holds);allocs++;return fail_alloc?NULL:malloc(n);}
"""
tests = r"""
int main(void){
 struct espconn socket={0};serbridgeConnData c={.conn=&socket};
 char data[6000];for(unsigned i=0;i<sizeof data;i++)data[i]=i%251;
 c.tx_failed=true;assert(!queueUartInput(&c,data,1));assert(!allocs&&!holds&&!input_stats.received);c.tx_failed=false;
 // Arm before SDK receive delivery, and re-arm immediately after credit.
 assert(holdUartInput(&c));assert(c.rx_held&&holds==1);
 assert(queueUartInput(&c,data,64));assert(holds==1&&c.rx_held&&allocs==1);
 assert(queueUartInput(&c,data+64,2144));assert(holds==1&&c.rxbufferlen==2208);
 assert(!memcmp(c.rxbuffer,data,2208));
 // Drained prefixes can be compacted without corrupting the retained suffix.
 c.rxbufferpos=1200;
 assert(queueUartInput(&c,data+2208,1072));assert(c.rxbufferlen==2080&&c.rxbufferpos==0);
 assert(!memcmp(c.rxbuffer,data+1200,2080));
 char before[MAX_RXBUFFER];memcpy(before,c.rxbuffer,2080);
 assert(!queueUartInput(&c,data,MAX_RXBUFFER));assert(closes==1&&input_stats.rejected==1);
 assert(!memcmp(before,c.rxbuffer,2080));free(c.rxbuffer);
 assert(renewUartInputCredit(&c));assert(unholds==1&&holds==2&&c.rx_held);
 fail_unhold=1;assert(!renewUartInputCredit(&c));
 assert(c.rx_held&&holds==2&&input_stats.hold_errors==1);fail_unhold=0;
 c=(serbridgeConnData){.conn=&socket};fail_hold=1;
 assert(!queueUartInput(&c,data,1));assert(!c.rxbuffer&&!c.rx_held&&input_stats.hold_errors==2);
 fail_hold=0;fail_alloc=1;
 assert(!queueUartInput(&c,data,1));assert(!c.rxbuffer&&input_stats.alloc_errors==1);
 puts("TCP input admission passed");
}
"""
output = root / ".build-tests"
output.mkdir(exist_ok=True)
test = output / "test-serbridge-input.c"
test.write_text(code + "\n" + macro + "\n" + source[start:end] + tests)
binary = output / "test-serbridge-input"
subprocess.run(
    [
        "gcc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        str(test),
        "-o",
        str(binary),
    ],
    check=True,
)
subprocess.run([str(binary)], check=True)
