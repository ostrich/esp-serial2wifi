#!/usr/bin/env python3
"""Compile complete Optiboot HTTP handlers, parser and wire producer with faults."""
import _paths
from pathlib import Path
import os,subprocess
ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'.build-tests/optiboot-http';OUT.mkdir(parents=True,exist_ok=True)
(OUT/'osapi.h').write_text('#pragma once\n')
source=r'''
#include <assert.h>
#include <esp8266.h>
#define os_strcpy strcpy
#define os_memmove memmove
#define os_printf(...) ((void)0)
uint32_t system_get_time(void);
void gpio_output_set(uint32_t,uint32_t,uint32_t,uint32_t);
#include "operation.c"
#include "api_request.c"
#include "cgioptiboot.c"
#include "pgmshared.c"
FlashConfig flashConfig;
void (*programmingCB)(char *,short);
static bool exclusive;
bool serbridgeHTTPExclusive(void){return exclusive;}
void serbridgeSetHTTPExclusive(bool on){exclusive=on;}
static int code,allocation,failAt;static unsigned resets,clients;static bool queued;
static char output[2601],body[8192],guard[8]="1",type[80]="application/x-www-form-urlencoded;charset=UTF-8";static size_t used;
static unsigned char flash[32768],command[256],reply[1024];
static unsigned commandLength,replyStart,replyEnd,address,readbacks,programs,lastBaud;
void *test_zalloc(size_t n){return ++allocation==failAt?NULL:calloc(1,n);}
int httpdSend(HttpdConnData *c,const char *p,int n){(void)c;if(n<0)n=strlen(p);assert(used+n<=2600);memcpy(output+used,p,n);used+=n;output[used]=0;return 1;}
void noCacheHeaders(HttpdConnData *c,int value){(void)c;code=value;}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdGetHeader(HttpdConnData *c,char *key,char *out,int n){(void)c;snprintf(out,n,"%s",!strcmp(key,"Content-Type")?type:guard);return strlen(out);}
int espconn_disconnect(struct espconn *c){(void)c;assert(0);return 0;}
void errorResponse(HttpdConnData *c,int value,char *message){(void)c;code=value;httpdSend(c,message,-1);}
uint32_t system_get_time(void){static uint32_t now;return now+=10000;}
void gpio_output_set(uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)a;(void)b;(void)c;(void)d;}
void makeGpio(uint8_t p){(void)p;}
void uart0_baud(int n){lastBaud=n;}
void uart0_config(uint8_t b,uint8_t p,uint8_t s){(void)b;(void)p;(void)s;}
void serbridgeReset(void){resets++;}
uint8_t serbridge_client_count(void){return clients;}
bool serialInputPending(void){return queued;}
int serbridgeInMCUFlashing(void){return 0;}
void os_timer_disarm(ETSTimer *t){t->armed=false;}
void os_timer_setfn(ETSTimer *t,void (*cb)(void *),void *a){t->callback=cb;t->arg=a;}
void os_timer_arm(ETSTimer *t,unsigned ms,bool repeat){assert(!repeat);t->armed=true;t->ms=ms;}
bool megaProgramPage(void){assert(0);return false;}
static void add(unsigned char c){assert(replyEnd<sizeof reply);reply[replyEnd++]=c;}
void uart0_write_char(char value){
 assert(commandLength<sizeof command);command[commandLength++]=(unsigned char)value;
 unsigned op=command[0],need=op==STK_LOAD_ADDRESS?4:op==STK_READ_PAGE?5:op==STK_LEAVE_PROGMODE?2:
  op==STK_PROG_PAGE&&commandLength>=3?5+((unsigned)command[1]<<8)+command[2]:0;
 if(!need||commandLength<need)return;assert(commandLength==need&&command[need-1]==CRC_EOP);
 if(op==STK_LOAD_ADDRESS){address=((unsigned)command[1]|((unsigned)command[2]<<8))*2;add(STK_INSYNC);add(STK_OK);}
 else if(op==STK_PROG_PAGE){unsigned n=((unsigned)command[1]<<8)|command[2];assert(address+n<=sizeof flash);memcpy(flash+address,command+4,n);address+=n;programs++;add(STK_INSYNC);add(STK_OK);}
 else if(op==STK_READ_PAGE){unsigned n=((unsigned)command[1]<<8)|command[2];readbacks++;add(STK_INSYNC);for(unsigned i=0;i<n;i++)add(flash[address+i]);add(STK_OK);}
 else if(op==STK_LEAVE_PROGMODE){add(STK_INSYNC);add(STK_OK);}
 commandLength=0;
}
uint16_t uart0_rx_poll(char *p,uint16_t n,uint32_t timeout){assert(timeout<=250000);unsigned available=replyEnd-replyStart;if(available>n)available=n;memcpy(p,reply+replyStart,available);replyStart+=available;return available;}
static struct espconn socket;
static HttpdPostData post;
static HttpdConnData conn;
static void setup(void){
 optibootInit();memset(&conn,0,sizeof conn);memset(&post,0,sizeof post);conn.conn=&socket;conn.post=&post;
 flashConfig.baud_rate=115200;flashConfig.data_bits=EIGHT_BITS;flashConfig.parity=ODD_BITS;flashConfig.stop_bits=TWO_STOP_BIT;flashConfig.reset_pin=12;
 resets=clients=allocation=failAt=0;queued=false;code=used=0;output[0]=0;strcpy(guard,"1");
 commandLength=replyStart=replyEnd=address=readbacks=programs=0;memset(flash,0xff,sizeof flash);
}
static void prepare(const char *text,int total){
 code=used=0;output[0]=0;conn.cgiPrivData=NULL;conn.requestType=HTTPD_METHOD_POST;strcpy(body,text);post.buff=body;post.buffLen=strlen(text);post.buffSize=sizeof body;post.received=post.buffLen;post.len=total<0?post.buffLen:total;
}
static void synced(void){progState=stateProg;baudRate=115200;optibootVers=0x0800;errMessage[0]=0;}
static void recordHex(char *out,unsigned address,unsigned length,unsigned seed){
 sprintf(out,":%02X%04X00",length,address);unsigned sum=length+(address>>8)+(address&255);
 for(unsigned i=0;i<length;i++){unsigned n=(seed+i)&255;sprintf(out+9+2*i,"%02X",n);sum+=n;}
 sprintf(out+9+length*2,"%02X",(-sum)&255);
}
static void fileHex(char *out){out[0]=0;char record[64];for(unsigned i=0;i<8;i++){recordHex(record,i*16,16,i*16);strcat(out,record);strcat(out,"\n");}strcat(out,":00000001FF\n");}
int main(void){
 setup();prepare("verify=readback",-1);strcpy(guard,"");cgiOptibootSync(&conn);assert(code==415&&!resets);strcpy(guard,"1");
 setup();prepare("verify=unknown",-1);cgiOptibootSync(&conn);assert(code==422&&!resets);
 setup();prepare("verify=readback",50);assert(cgiOptibootSync(&conn)==HTTPD_CGI_MORE&&!resets);
 setup();clients=1;prepare("verify=readback",-1);cgiOptibootSync(&conn);assert(code==409&&!resets);
 setup();queued=true;prepare("verify=readback",-1);cgiOptibootSync(&conn);assert(code==409&&!resets);
 setup();prepare("verify=readback",-1);cgiOptibootSync(&conn);assert(code==204&&resets==1&&verifyFlash&&exclusive);
 cgiOptibootSync(&conn);assert(code==409&&resets==1&&exclusive);synced();
 conn.requestType=HTTPD_METHOD_GET;used=0;cgiOptibootSync(&conn);assert(strstr(output,"; readback enabled"));
 char hex[1024];fileHex(hex);prepare(hex,-1);cgiOptibootData(&conn);
 assert(code==200&&strstr(output,"Success. 128 bytes at 115200 baud; readback verified 128 bytes."));
 assert(programs==1&&readbacks==1&&!optibootData&&!programmingCB&&lastBaud==115200);
 for(unsigned i=0;i<128;i++)assert(flash[i]==(uint8_t)i);
 setup();prepare("",-1);cgiOptibootSync(&conn);assert(code==204&&!verifyFlash);synced();prepare(hex,-1);cgiOptibootData(&conn);assert(code==200&&strstr(output,"Success.")&&!readbacks);
 for(int fail=1;fail<=3;fail++){
  setup();prepare("verify=readback",-1);cgiOptibootSync(&conn);synced();allocation=0;failAt=fail;prepare(hex,-1);cgiOptibootData(&conn);assert(code==503&&!optibootData&&!programmingCB);
 }
 setup();synced();prepare("\n",100);assert(cgiOptibootData(&conn)==HTTPD_CGI_MORE&&optibootData&&optibootData->conn==&conn);
 struct HttpdConnData other=conn;other.cgiPrivData=NULL;code=0;cgiOptibootData(&other);assert(code==409&&optibootData->conn==&conn);
 conn.conn=NULL;cgiOptibootData(&conn);assert(!optibootData&&!programmingCB);
 setup();synced();char invalid[600];memset(invalid,'x',sizeof invalid-1);invalid[sizeof invalid-1]=0;prepare(invalid,-1);cgiOptibootData(&conn);assert(code==400&&!optibootData&&!programs);
 setup();synced();strcat(hex,"garbage");prepare(hex,-1);cgiOptibootData(&conn);assert(code==400&&!optibootData);fileHex(hex);
 setup();conn.requestType=HTTPD_METHOD_GET;cgiOptibootData(&conn);assert(code==405&&!optibootData);
 char pretty[16]="",raw[4]={0,(char)255,'A','\n'};appendPretty(pretty,sizeof pretty,raw,sizeof raw);assert(!strcmp(pretty,"\\x00\\xffA\\n"));
 for(int cap=1;cap<=16;cap++){char guarded[20];memset(guarded,0xa5,sizeof guarded);guarded[0]=0;appendPretty(guarded,cap,raw,sizeof raw);assert((unsigned char)guarded[cap]==0xa5);}
 optibootInit();puts("PASS complete Optiboot HTTP: guarded readback sync, raw file/result/wire bytes, legacy mode, every allocation failure, upload ownership/abort, malformed records/trailing data and bounded diagnostics");
}
'''
(OUT/'test.c').write_text(source)
subprocess.run(['cc','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-variable','-Wno-ignored-qualifiers','-Wno-misleading-indentation','-funsigned-char','-fsanitize=address,undefined',
 *['-I'+str(p) for p in [OUT,ROOT/'tests/support/api-stubs',ROOT/'src/core',ROOT/'src/httpd',ROOT/'src/serial',ROOT/'include']],str(OUT/'test.c'),str(ROOT/'src/core/api_json.c'),'-o',str(OUT/'test')],check=True)
subprocess.run([str(OUT/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
