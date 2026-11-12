#!/usr/bin/env python3
"""Actual UART/capture producers and serial APIs with independent byte oracles."""
import _paths
import base64,json,os,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'.build-tests/serial-api';OUT.mkdir(parents=True,exist_ok=True)
console=(ROOT/'src/serial/console.c').read_text()
ring=console[console.index('#define BUF_MAX'):console.index('int ICACHE_FLASH_ATTR\najaxConsoleReset(')]
source=r'''
#include <assert.h>
#include <stdlib.h>
#include <esp8266.h>
#define os_strncmp strncmp
static void *allocate(size_t n);
static unsigned streamHeap=24000;
uint32_t system_get_free_heap_size(void){return streamHeap;}
#define os_malloc allocate
#include "console.h"
''' + ring + r'''
static unsigned now=1,waits;
unsigned system_get_time(void){return now;}
#include "operation.c"
#include "api_request.c"
#include "serial_api.c"
FlashConfig flashConfig;
void (*programmingCB)(char *,short);
static bool failSave,failAlloc,failSend;static unsigned disconnects;static unsigned writes,formats,resets,room=100,txlen;
static char tx[1024],output[2601];static size_t used;static int code;
static char guard[8]="1",type[80]="application/x-www-form-urlencoded;charset=UTF-8";
static void *allocate(size_t n){return failAlloc?NULL:malloc(n);}
void *test_zalloc(size_t n){return failAlloc?NULL:calloc(1,n);}
void httpdStartResponse(HttpdConnData *c,int value){(void)c;code=value;}
void httpdReleaseHeaders(HttpdConnData *c){(void)c;}
void httpdWait(HttpdConnData *c,unsigned ms){(void)c;assert(ms==30);waits++;}
int httpdSend(HttpdConnData *c,const char *p,int n){(void)c;if(failSend)return 0;if(n<0)n=strlen(p);assert(used+n<=2600);memcpy(output+used,p,n);used+=n;output[used]=0;return 1;}
void noCacheHeaders(HttpdConnData *c,int value){(void)c;code=value;}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdGetHeader(HttpdConnData *c,char *key,char *out,int n){(void)c;snprintf(out,n,"%s",!strcmp(key,"Content-Type")?type:guard);return strlen(out);}
int espconn_disconnect(struct espconn *c){(void)c;assert(failSend);disconnects++;return 0;}
bool configSave(void){writes++;return !failSave;}
void uart0_config(uint8_t b,uint8_t p,uint8_t s){assert(b==flashConfig.data_bits&&p==flashConfig.parity&&s==flashConfig.stop_bits);formats++;}
void uart0_baud(int n){assert(n==flashConfig.baud_rate);formats++;}
unsigned uart0_tx_available(void){return room;}
void uart0_tx_buffer(char *p,uint16 n){assert(n<=room&&txlen+n<=sizeof tx);memcpy(tx+txlen,p,n);txlen+=n;}
uint8_t serbridge_listener_mask(void){return 3;}
static unsigned clients=2;
uint8_t serbridge_client_count(void){return clients;}
int serbridgeInMCUFlashing(void){return 0;}
void serbridgeReset(void){resets++;}
void os_timer_disarm(ETSTimer *t){t->armed=false;}
void os_timer_setfn(ETSTimer *t,void (*cb)(void *),void *arg){t->callback=cb;t->arg=arg;}
void os_timer_arm(ETSTimer *t,unsigned ms,bool repeat){assert(!repeat);t->armed=true;t->ms=ms;}
static struct espconn socket;
static HttpdPostData post;
static HttpdConnData conn={.conn=&socket,.post=&post,.url="/api/v1/serial/capture"};
static int request(int (*fn)(HttpdConnData *),const char *body){
 code=0;used=0;output[0]=0;conn.requestType=body?HTTPD_METHOD_POST:HTTPD_METHOD_GET;
 post.buff=(char *)body;post.len=post.buffLen=post.received=body?strlen(body):0;fn(&conn);return code;
}
static void dummy(char *p,short n){(void)p;(void)n;}
static void fire(void){assert(sendTimer.armed);sendTimer.armed=false;sendTimer.callback(NULL);}
int main(void){
 console_boot=42;console_epoch=0;ConsoleCapture w;
 assert(request(apiSerialCapture,NULL)==200);printf("empty\t%s\n",output);
 for(unsigned i=0;i<1300;i++)console_write_char((char)i);
 consoleCapture(&w,false,0,0,0);assert(w.start==277&&w.end==1300&&w.length==1023&&!w.reason);
 for(unsigned i=0;i<w.length;i++)assert(consoleCaptureByte(&w,i)==(uint8_t)(277+i));
 assert(request(apiSerialCapture,NULL)==200);printf("binary\t%s\n",output);
 conn.url="/api/v1/serial/capture/42/0/0";assert(request(apiSerialCapture,NULL)==200);printf("gap\t%s\n",output);
 conn.url="/api/v1/serial/capture/42/0/1299";assert(request(apiSerialCapture,NULL)==200);printf("last\t%s\n",output);
 conn.url="/api/v1/serial/capture/42/0/1300";assert(request(apiSerialCapture,NULL)==200);printf("caught-up\t%s\n",output);
 const char *bad[]={"/api/v1/serial/capture/","/api/v1/serial/capture/42/0/1/","/api/v1/serial/capture/42/0/-1","/api/v1/serial/capture/42/0/4294967296","/api/v1/serial/capture/x/0/1","/api/v1/serial/capture/42/0"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){conn.url=(char*)bad[i];assert(request(apiSerialCapture,NULL)==422);}
 conn.url="/api/v1/serial/capture";conn.getArgs="cursor=1";assert(request(apiSerialCapture,NULL)==405);conn.getArgs=NULL;
 console_pos=UINT32_MAX-1022;consoleCapture(&w,false,0,0,0);assert(w.end==0);
 consoleCapture(&w,true,42,0,UINT32_MAX-1);assert(w.start==UINT32_MAX-1&&w.end==0&&w.length==2&&!w.reason);
 console_write_char('x');consoleCapture(&w,true,42,0,0);assert(w.start==0&&w.end==1&&w.length==1&&consoleCaptureByte(&w,0)=='x');
 consoleCapture(&w,true,42,0,100);assert(!strcmp(w.reason,"cursor_unavailable"));
 consoleCaptureClear();consoleCapture(&w,true,42,0,1);assert(!strcmp(w.reason,"cleared")&&!w.length);
 consoleCapture(&w,true,41,1,0);assert(!strcmp(w.reason,"restart"));
 flashConfig.baud_rate=115200;flashConfig.data_bits=EIGHT_BITS;flashConfig.parity=NONE_BITS;flashConfig.stop_bits=ONE_STOP_BIT;flashConfig.reset_pin=-1;
 const char *valid="baud=9600&data_bits=5&parity=O&stop_bits=2";
 assert(request(apiSerialUART,"baud=9600x&data_bits=5&parity=O&stop_bits=2")==422&&!writes&&!formats);
 assert(request(apiSerialUART,"baud=9600&data_bits=5&parity=X&stop_bits=2")==422&&!writes);
 failSave=true;assert(request(apiSerialUART,valid)==503&&flashConfig.baud_rate==115200&&flashConfig.data_bits==EIGHT_BITS&&!formats);
 failSave=false;assert(request(apiSerialUART,valid)==200&&flashConfig.baud_rate==9600&&flashConfig.data_bits==FIVE_BITS&&flashConfig.parity==ODD_BITS&&flashConfig.stop_bits==TWO_STOP_BIT&&formats==2);
 printf("uart\t%s\n",output);
 assert(request(apiSerialUART,"baud=9600&data_bits=5&parity=O&stop_bits=1.5")==200&&flashConfig.stop_bits==ONE_HALF_STOP_BIT);
 programmingCB=dummy;assert(request(apiSerialUART,valid)==409);assert(request(apiSerialSend,"hex=00")==409);programmingCB=NULL;
 assert(request(apiSerialSend,"hex=0")==422&&!txlen);assert(request(apiSerialSend,"hex=00zz")==422&&!txlen);
 failAlloc=true;assert(request(apiSerialSend,"hex=00")==503&&!sendBuffer);failAlloc=false;
 assert(request(apiSerialSend,"hex=000d0aff")==200&&!txlen&&sendLength==4);
 assert(request(apiSerialUART,valid)==409);assert(request(apiSerialSend,"hex=41")==409);
 room=0;fire();assert(!txlen&&sendTimer.armed);room=1;fire();assert(txlen==1&&sendTimer.armed);room=3;fire();
 assert(txlen==4&&!sendBuffer&&!memcmp(tx,"\x00\x0d\x0a\xff",4));
 assert(request(apiSerialSend,"hex=4142")==200);programmingCB=dummy;fire();assert(!sendBuffer&&sendCancelled==2&&txlen==4);programmingCB=NULL;
 assert(request(apiSerialReset,"action=reset_mcu")==409&&!resets);flashConfig.reset_pin=12;
 assert(request(apiSerialReset,"action=reset_mcu")==200&&resets==1);
 console_write_char('a');assert(request(apiSerialClear,"action=clear_capture")==200);consoleCapture(&w,false,0,0,0);assert(!w.length);

 conn.url="/api/v1/serial/live";console_write_char('H');
 assert(request(apiSerialLive,NULL)==409&&!liveOwner);clients=0;
 failAlloc=true;assert(request(apiSerialLive,NULL)==503&&!liveOwner);failAlloc=false;
 assert(request(apiSerialLive,"hex=41")==405&&!liveOwner);
 failSend=true;assert(request(apiSerialLive,NULL)==200&&!liveOwner&&!conn.cgiData&&disconnects==1);failSend=false;
 assert(request(apiSerialLive,NULL)==200&&liveOwner==&conn&&strstr(output,"event: ready"));
 assert(strstr(output,"\"data\":\"SA==\"")); // saved H is output-only history
 assert(stream_start==1&&stream_end==1&&!consoleStreamPending());
 HttpdConnData other=conn;other.cgiData=NULL;used=0;assert(apiSerialLive(&other)==HTTPD_CGI_DONE&&code==409&&!other.cgiData);
 other.conn=NULL;apiSerialLive(&other);assert(liveOwner==&conn);
 used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_MORE&&waits==1&&!used);
 const char liveBytes[]="\x1b[31mX\r\n";for(unsigned i=0;i<sizeof liveBytes-1;i++)console_write_char(liveBytes[i]);
 used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_MORE&&strstr(output,"event: output")&&strstr(output,"G1szMW1YDQo=")&&strstr(output,"\"start\":1")&&strstr(output,"\"end\":9"));
 used=0;now+=1000000;apiSerialLive(&conn);assert(strstr(output,": alive"));
 // A listing larger than rolling capture is retained in the live FIFO.
 for(unsigned i=0;i<4096;i++)console_write_char(i%256);
 assert(consoleStreamPending()==4096&&consoleStreamHighWater()==4096);
 unsigned total=0;
 while(consoleStreamPending()){
  ConsoleCapture live;consoleStreamCapture(&live);assert(!live.reason&&live.length<=1023);
  for(unsigned i=0;i<live.length;i++)assert(consoleCaptureByte(&live,i)==(uint8_t)((total+i)%256));
  unsigned n=live.length;used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_MORE&&used<1800);total+=n;
 }
 assert(total==4096&&!consoleStreamLost());
 // An ACK pause > the old ring's retention does not overwrite live bytes.
 for(unsigned i=0;i<2304;i++)console_write_char('a');
 consoleCapture(&w,false,0,0,0);assert(w.length==1023);
 assert(consoleStreamPending()==2304);while(consoleStreamPending()){used=0;apiSerialLive(&conn);}
 // Clear changes the generation and closes the stream explicitly.
 consoleCaptureClear();used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_DONE&&strstr(output,"\"reason\":\"cleared\""));assert(!liveOwner);
 // Exhausting the bounded FIFO fails closed instead of silently overwriting it.
 assert(request(apiSerialLive,NULL)==200);
 for(unsigned i=0;i<9000;i++)console_write_char('z');used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_DONE&&strstr(output,"\"reason\":\"gap\""));assert(!liveOwner&&!consoleStreamPending()&&consoleStreamLost()==9000&&consoleStreamTotalLost()==9000);
 conn.conn=NULL;apiSerialLive(&conn);assert(!liveOwner&&!conn.cgiData);conn.conn=&socket;
 assert(request(apiSerialLive,NULL)==200);assert(!consoleStreamLost()&&consoleStreamTotalLost()==9000);failAlloc=true;console_write_char('m');failAlloc=false;used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_DONE&&strstr(output,"\"reason\":\"gap\""));assert(!liveOwner);
 assert(consoleStreamTotalLost()==9001);assert(request(apiSerialLive,NULL)==200);streamHeap=6000;console_write_char('m');streamHeap=24000;used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_DONE&&strstr(output,"\"reason\":\"gap\""));assert(!liveOwner);
 assert(consoleStreamTotalLost()==9002);assert(request(apiSerialLive,NULL)==200);programmingCB=dummy;used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_DONE&&strstr(output,"event: stopped")&&!liveOwner&&!conn.cgiData);programmingCB=NULL;
 assert(request(apiSerialLive,NULL)==200);clients=1;used=0;assert(apiSerialLive(&conn)==HTTPD_CGI_DONE&&strstr(output,"event: stopped")&&!liveOwner&&!conn.cgiData);clients=0;
 consoleCaptureClear();assert(consoleStreamTotalLost()==9002);
 strcpy(guard,"");unsigned before=writes;assert(request(apiSerialUART,valid)==415&&writes==before);strcpy(guard,"1");
 conn.conn=NULL;before=writes;apiSerialUART(&conn);assert(writes==before);
}
'''
(OUT/'test.c').write_text(source)
subprocess.run(['cc','-std=c11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',
 '-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-ignored-qualifiers',
 *['-I'+str(ROOT/p) for p in ['tests/api-stubs','src/core','src/httpd','src/serial','include']],
 str(OUT/'test.c'),str(ROOT/'src/core/api_json.c'),'-o',str(OUT/'test')],check=True)
output=subprocess.check_output([str(OUT/'test')],text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
cases={name:json.loads(body) for name,body in (line.split('\t',1) for line in output.splitlines())}
assert cases['empty']['data']=='' and cases['empty']['reason'] is None
assert base64.b64decode(cases['binary']['data'])==bytes(i%256 for i in range(277,1300))
assert cases['gap']['reason']=='gap' and cases['gap']['lost']==277
assert base64.b64decode(cases['last']['data'])==bytes([1299%256])
assert cases['caught-up']['data']==''
assert cases['uart']['parity']=='O' and cases['uart']['data_bits']==5 and cases['uart']['stop_bits']=='2'
(OUT/'responses.json').write_text(json.dumps(cases,indent=2)+'\n')
print('PASS actual serial APIs: independent FIFO retains 4KiB listing and ACK stalls, bounded overflow/OOM/clear/abort/exclusivity, raw capture/cursors, UART rollback and paced input')
