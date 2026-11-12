#!/usr/bin/env python3
"""Actual Optiboot page sender against a deterministic STK500 flash peer."""

import _paths
from source_text import SourceText
from pathlib import Path
import os, re, subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/optiboot-readback"
OUT.mkdir(parents=True, exist_ok=True)
s = SourceText((ROOT / "src/core/cgioptiboot.c").read_text())
functions = s[s.index("// Poll UART for ACKs") : s.index("//===== Rebooting and getting sync")]
h = SourceText((ROOT / "src/core/pgmshared.h").read_text())
record = re.search(r"extern struct optibootData \{.*?\} \*optibootData;", h, re.S).group()
source = (
    r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "stk500.h"
#define ICACHE_FLASH_ATTR
#define DBG(...) ((void)0)
#define os_memcmp memcmp
#define os_memmove memmove
#define os_sprintf sprintf
#define os_strcpy strcpy
#define PGM_TIMEOUT 20000
typedef struct {int unused;} HttpdConnData;
"""
    + record
    + r"""
struct optibootData *optibootData;
static struct optibootData state;
static char page[768],errMessage[128];
static short ackWait;
static bool verifyFlash;
static uint32_t verifiedBytes,baudRate;
static unsigned char flash[32768],command[256],reply[1024];
static unsigned commandLength,replyStart,replyEnd,address,reads,programs;
static unsigned fault;static uint32_t pollTimeout;
static void armTimer(uint32_t ms){assert(ms==PGM_TIMEOUT);}
static void add(unsigned char c){assert(replyEnd<sizeof reply);reply[replyEnd++]=c;}
static void uart0_write_char(char value){
 unsigned char c=value;assert(commandLength<sizeof command);command[commandLength++]=c;
 unsigned op=command[0],need=op==STK_LOAD_ADDRESS?4:op==STK_READ_PAGE?5:
  op==STK_PROG_PAGE&&commandLength>=3?5+((unsigned)command[1]<<8)+command[2]:0;
 if(!need||commandLength<need)return;
 assert(commandLength==need&&command[need-1]==CRC_EOP);
 if(op==STK_LOAD_ADDRESS){address=((unsigned)command[1]|((unsigned)command[2]<<8))*2;add(STK_INSYNC);add(fault==3?0:STK_OK);}
 else if(op==STK_PROG_PAGE){unsigned n=((unsigned)command[1]<<8)|command[2];assert(address+n<=sizeof flash);memcpy(flash+address,command+4,n);address+=n;programs++;add(STK_INSYNC);add(STK_OK);}
 else {unsigned n=((unsigned)command[1]<<8)|command[2];reads++;add(STK_INSYNC);for(unsigned i=0;i<n;i++)add(flash[address+i]^((fault==1&&i==7)?1:0));if(fault!=2)add(STK_OK);}
 commandLength=0;
}
static uint16_t uart0_rx_poll(char *out,uint16_t need,uint32_t timeout){
 pollTimeout=timeout;unsigned available=replyEnd-replyStart;if(available>need)available=need;
 memcpy(out,reply+replyStart,available);replyStart+=available;return available;
}
"""
    + functions
    + r"""
static void setup(bool verify,unsigned problem){
 memset(&state,0,sizeof state);memset(flash,0xff,sizeof flash);for(unsigned i=0;i<sizeof page;i++)page[i]=i;
 optibootData=&state;state.pageBuf=page;state.pageLen=256;state.pgmSz=128;
 verifiedBytes=ackWait=commandLength=replyStart=replyEnd=reads=programs=0;errMessage[0]=0;baudRate=115200;verifyFlash=verify;fault=problem;
}
int main(void){
 setup(true,0);assert(optibootProgramPage());assert(state.pageLen==128&&state.pgmDone==128&&verifiedBytes==128&&reads==1);
 assert(optibootProgramPage());assert(!state.pageLen&&state.pgmDone==256&&verifiedBytes==256&&reads==2);
 for(unsigned i=0;i<256;i++)assert(flash[i]==(uint8_t)i);
 setup(false,0);assert(optibootProgramPage()&&programs==1&&!reads&&!verifiedBytes);
 setup(true,1);assert(!optibootProgramPage()&&programs==1&&reads==1&&!state.pgmDone&&!verifiedBytes&&strstr(errMessage,"readback"));
 setup(true,2);assert(!optibootProgramPage()&&!verifiedBytes);
 setup(true,3);assert(!optibootProgramPage()&&!programs);
 setup(true,0);baudRate=9600;assert(optibootProgramPage()&&pollTimeout>=200000&&pollTimeout<=250000);
 setup(true,0);state.address=0x7e00;assert(!optibootProgramPage()&&!programs);
 setup(true,0);state.address=1;assert(!optibootProgramPage()&&!programs);
 setup(true,0);baudRate=300;assert(!optibootProgramPage()&&!programs);
 setup(false,0);ackWait=2;add(STK_INSYNC);add(STK_OK);add(STK_INSYNC);add(0);assert(!pollAck());
 puts("PASS actual Optiboot page wire protocol: independent flash bytes/readback, mismatch/truncation/ACK errors, legacy mode, bounded baud timeout and application/alignment guards");
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
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-I" + str(ROOT / "src/core"),
        str(OUT / "test.c"),
        "-o",
        str(OUT / "test"),
    ],
    check=True,
)
subprocess.run(
    [str(OUT / "test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"}
)
