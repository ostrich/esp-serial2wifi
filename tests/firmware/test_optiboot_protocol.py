#!/usr/bin/env python3
"""Actual Optiboot receive state machine; no ESP emulation or target hardware."""
import _paths
from pathlib import Path
import os,subprocess,sys
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'.build-tests/optiboot-protocol';OUT.mkdir(parents=True,exist_ok=True)
s=(Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'src/core/cgioptiboot.c').read_text()
globals=s[s.index('static enum {'):s.index('#define MAX_SAVED')]
receive=s[s.index('// skip in-sync responses'):]
source=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "stk500.h"
#define ICACHE_FLASH_ATTR
#define DBG(...) ((void)0)
#define os_memcpy memcpy
#define os_memmove memmove
#define os_sprintf sprintf
#define RESP_SZ 64
#define PGM_INTERVAL 200
char responseBuf[RESP_SZ],errMessage[128];short responseLen;
'''+globals+r'''
static unsigned sent,resets;
static void uart0_write_char(char c){(void)c;sent++;}
static void armTimer(uint32_t ms){assert(ms==PGM_INTERVAL);}
static void optibootInit(void){resets++;progState=stateInit;responseLen=0;errMessage[0]=0;}
'''+receive+r'''
static void feed(const unsigned char *p,unsigned n){optibootUartRecv((char*)p,n);}
int main(int argc,char **argv){
 (void)argv;
 const unsigned char ack[]={STK_INSYNC,STK_OK},sig[]={STK_INSYNC,0x1e,0x95,0x0f,STK_OK};
 if(argc>1)goto rejected;
 progState=stateSync;feed(ack,1);assert(progState==stateSync);feed(ack+1,1);assert(progState==stateGetSig&&sent==2);
 feed(sig,2);assert(progState==stateGetSig);feed(sig+2,3);assert(progState==stateGetVersLo&&sent==5);
 const unsigned char low[]={STK_INSYNC,0,STK_OK},high[]={STK_INSYNC,8,STK_OK};
 feed(low,3);assert(progState==stateGetVersHi&&optibootVers==0);feed(high,3);assert(progState==stateProg&&optibootVers==0x0800);
 ackWait=2;const unsigned char two[]={STK_INSYNC,STK_OK,STK_INSYNC,STK_OK};feed(two,sizeof two);assert(!ackWait&&!responseLen);
rejected:
 responseLen=0;progState=stateGetSig;const unsigned char bad[]={STK_INSYNC,0x1e,0x94,0x06,STK_OK};feed(bad,5);
 assert(progState==stateInit&&resets==1&&strstr(errMessage,"signature"));
 char repeated[32];for(unsigned i=0;i<30;i+=2){repeated[i]=STK_INSYNC;repeated[i+1]=STK_OK;}repeated[30]=7;
 assert(skipInSync(repeated,31)==1&&repeated[0]==7);
 puts("PASS actual Optiboot receive states: split packets, zero minor version, binary signature, page ACKs, rejected target and overlapping prefix removal");
}
'''
(OUT/'test.c').write_text(source)
subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-variable','-funsigned-char','-fsanitize=address,undefined','-I'+str(ROOT/'src/core'),str(OUT/'test.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')]+sys.argv[2:],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
