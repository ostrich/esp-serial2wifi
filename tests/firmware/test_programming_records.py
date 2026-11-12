#!/usr/bin/env python3
"""Actual shared Intel HEX producer: bounded pages and malformed records."""
import _paths
from pathlib import Path
import os,subprocess,sys,re
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'.build-tests/programming-records';OUT.mkdir(parents=True,exist_ok=True)
path=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'src/core/pgmshared.c'
s=path.read_text();body=s[s.index('struct optibootData *optibootData;'):]
h=(ROOT/'src/core/pgmshared.h').read_text();record=re.search(r'extern struct optibootData \{.*?\} \*optibootData;',h,re.S).group()
source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ICACHE_FLASH_ATTR
#define DBG(...) ((void)0)
#define os_sprintf sprintf
#define os_strcpy strcpy
#define RESP_SZ 64
#define ERR_MAX 128
#define PGM_PAGE_BYTES 512
#define PGM_PAGE_BUFFER_BYTES 768
typedef struct {int unused;} HttpdConnData;
''' +record+r'''
bool megaProgramPage(void);
bool optibootProgramPage(void);
'''+body+r'''
static unsigned written;
static uint8_t oracle[65536];
static bool failPage;
static bool program(void){
 if(failPage)return false;
 unsigned n=optibootData->pageLen;if(n>optibootData->pgmSz)n=optibootData->pgmSz;
 assert(n&&written+n<=sizeof oracle);memcpy(oracle+written,optibootData->pageBuf,n);written+=n;
 memmove(optibootData->pageBuf,optibootData->pageBuf+n,optibootData->pageLen-n);optibootData->pageLen-=n;optibootData->address+=n;return true;
}
bool megaProgramPage(void){return program();}
bool optibootProgramPage(void){return program();}
static void recordHex(char *out,unsigned type,unsigned addr,unsigned length,unsigned seed){
 sprintf(out,":%02X%04X%02X",length,addr,type);unsigned sum=length+(addr>>8)+(addr&255)+type;
 for(unsigned i=0;i<length;i++){unsigned n=(seed+i)&255;sprintf(out+9+2*i,"%02X",n);sum+=n;}
 sprintf(out+9+length*2,"%02X",(-sum)&255);
}
int main(void){
 struct optibootData state={0};optibootData=&state;state.pageBuf=malloc(PGM_PAGE_BUFFER_BYTES);assert(state.pageBuf);
 char record[524];
 for(unsigned page=128;page<=512;page*=2){
  memset(&state,0,sizeof state);state.pageBuf=malloc(PGM_PAGE_BUFFER_BYTES);assert(state.pageBuf);state.pgmSz=page;written=0;
  for(unsigned i=0;i<20;i++){recordHex(record,0,i*255,255,i*255);assert(processRecord(record,strlen(record)));}
  recordHex(record,1,0,0,0);assert(processRecord(record,strlen(record))&&state.eof&&written==5100);
  for(unsigned i=0;i<written;i++)assert(oracle[i]==(uint8_t)i);
  free(state.pageBuf);
 }
 state.pageBuf=malloc(PGM_PAGE_BUFFER_BYTES);state.pgmSz=128;state.pageLen=0;state.eof=false;
 const char *invalid[]={"",":",":00000001FE",":0100000100FE",":00000101FE",":00000003FD",":0200000400FC"};
 for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){strcpy(record,invalid[i]);assert(!processRecord(record,strlen(record)));}
 recordHex(record,0,0,255,0);record[strlen(record)-1]='0';record[strlen(record)-2]='0';assert(!processRecord(record,strlen(record)));assert(strlen(errMessage)<ERR_MAX);
 state.eof=false;state.pageLen=0;recordHex(record,0,0,255,0);failPage=true;assert(!processRecord(record,strlen(record)));failPage=false;
 free(state.pageBuf);puts("PASS actual HEX producer: long records drain all full pages (128/256/512), exact byte oracle, malformed lengths/checksum/type and programmer failures");
}
'''
# No leaked initial allocation: each loop allocates its own guarded producer buffer.
source=source.replace('state.pageBuf=malloc(PGM_PAGE_BUFFER_BYTES);assert(state.pageBuf);\n char record', 'char record')
(OUT/'test.c').write_text(source)
subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(OUT/'test.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
