#!/usr/bin/env python3
"""Real backend modules with bounded frames, borrowed sends and reordered callbacks."""
import _paths
from pathlib import Path
import os,subprocess
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'.build-tests/backend-modules';OUT.mkdir(parents=True,exist_ok=True)
SDK=r'''
#ifndef BACKEND_SDK
#define BACKEND_SDK
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define ICACHE_RODATA_ATTR
#define os_printf(...) ((void)0)
#define LOCAL static
#define TRUE true
#define FALSE false
typedef uint8_t uint8;typedef uint16_t uint16;typedef uint32_t uint32;typedef int8_t sint8;typedef int16_t sint16;typedef int32_t sint32;
typedef struct ip_addr {uint32_t addr;} ip_addr_t;
typedef struct {void (*callback)(void*);void *arg;bool armed;} ETSTimer;
typedef void (*espconn_connect_callback)(void*);
typedef void (*espconn_reconnect_callback)(void*,sint8);
typedef void (*espconn_recv_callback)(void*,char*,unsigned short);
typedef void (*espconn_sent_callback)(void*);
typedef struct {unsigned remote_port,local_port;uint8_t remote_ip[4],local_ip[4];} esp_tcp;
typedef esp_tcp esp_udp;
struct espconn {unsigned type,state,link_cnt;union {esp_tcp *tcp;esp_udp *udp;} proto;void *reverse;
 espconn_connect_callback connect_cb,discon_cb;espconn_reconnect_callback recon_cb;espconn_recv_callback recv_cb;espconn_sent_callback sent_cb;
 const uint8_t *borrowed;unsigned borrowed_length;bool closing,registered;};
#define ESPCONN_NONE 0
#define ESPCONN_TCP 1
#define ESPCONN_UDP 2
#define ESPCONN_OK 0
#define ESPCONN_INPROGRESS -5
#define ESPCONN_ABRT -3
#define ESPCONN_MEM -1
#define STATIONAP_MODE 3
static unsigned allocations,allocateAt,allocationCalls;
static void *test_alloc(size_t n){if(++allocationCalls==allocateAt)return NULL;void *p=malloc(n?n:1);assert(p);allocations++;return p;}
static void *test_zalloc(size_t n){void *p=test_alloc(n);if(p)memset(p,0,n);return p;}
static void test_free(void *p){if(p){assert(allocations);allocations--;free(p);}}
#define os_malloc test_alloc
#define os_zalloc test_zalloc
#define os_free test_free
#define os_memcpy memcpy
#define os_memmove memmove
#define os_memset memset
#define os_memcmp memcmp
#define os_strlen strlen
#define os_strcmp strcmp
#define os_strncmp strncmp
#define os_strncpy strncpy
#define os_strcpy strcpy
#define os_strchr strchr
#define os_sprintf sprintf
static unsigned sends,connects;static int sendError,connectError,dnsMode=ESPCONN_INPROGRESS;
static struct {struct espconn *conn;void (*callback)(const char*,ip_addr_t*,void*);} dns[16];static unsigned dnsCount;
static uint8_t wire[40000];static unsigned wireUsed;
static uint16_t espconn_port(void){static unsigned port=2000;return ++port;}
static void espconn_regist_connectcb(struct espconn *p,espconn_connect_callback cb){p->connect_cb=cb;}
static void espconn_regist_reconcb(struct espconn *p,espconn_reconnect_callback cb){p->recon_cb=cb;}
static void espconn_regist_disconcb(struct espconn *p,espconn_connect_callback cb){p->discon_cb=cb;}
static void espconn_regist_sentcb(struct espconn *p,espconn_sent_callback cb){p->sent_cb=cb;}
static void espconn_regist_recvcb(struct espconn *p,espconn_recv_callback cb){p->recv_cb=cb;}
static int espconn_regist_time(struct espconn *p,unsigned n,int type){(void)p;(void)n;(void)type;return 0;}
static int espconn_connect(struct espconn *p){connects++;p->registered=!connectError;return connectError;}
static int espconn_accept(struct espconn *p){p->registered=true;return 0;}
static int espconn_create(struct espconn *p){p->registered=true;return 0;}
static int espconn_delete(struct espconn *p){p->registered=false;p->borrowed=NULL;return 0;}
static int espconn_tcp_set_max_con_allow(struct espconn *p,unsigned n){(void)p;(void)n;return 0;}
static int espconn_disconnect(struct espconn *p){assert(p);p->closing=true;return 0;}
static int espconn_sent(struct espconn *p,uint8_t *data,unsigned n){assert(p&&!p->borrowed&&n);p->borrowed=data;p->borrowed_length=n;sends++;return sendError;}
static int espconn_send(struct espconn *p,uint8_t *data,unsigned n){return espconn_sent(p,data,n);}
static int espconn_gethostbyname(struct espconn *p,const char *host,ip_addr_t *ip,void (*cb)(const char*,ip_addr_t*,void*)){
 (void)host;if(dnsMode==0)ip->addr=0x01020304;else if(dnsMode==ESPCONN_INPROGRESS){assert(dnsCount<16);dns[dnsCount].conn=p;dns[dnsCount++].callback=cb;}return dnsMode;
}
static void wifi_set_broadcast_if(unsigned n){(void)n;}
static void os_timer_disarm(ETSTimer *t){t->armed=false;}
static void os_timer_setfn(ETSTimer *t,void (*cb)(void*),void *arg){t->callback=cb;t->arg=arg;}
static void os_timer_arm(ETSTimer *t,unsigned n,bool repeat){(void)n;(void)repeat;t->armed=true;}
static uint8_t UTILS_StrToIP(const char *text,void *out){if(!strcmp(text,"1.2.3.4")||!strcmp(text,"0.0.0.0")){memset(out,1,4);return 1;}return 0;}
static void connected(struct espconn *p){assert(p->connect_cb);p->connect_cb(p);}
static void ack(struct espconn *p){assert(p->borrowed);assert(wireUsed+p->borrowed_length<=sizeof wire);memcpy(wire+wireUsed,p->borrowed,p->borrowed_length);wireUsed+=p->borrowed_length;p->borrowed=NULL;p->sent_cb(p);}
static void terminal(struct espconn *p,bool reset){p->borrowed=NULL;p->registered=false;if(reset)p->recon_cb(p,-3);else p->discon_cb(p);}
static void resolved(unsigned n,bool okay){assert(n<dnsCount);ip_addr_t ip={0x01020304};dns[n].callback("test",okay?&ip:NULL,dns[n].conn);}
#endif
'''
(OUT/'esp8266.h').write_text(SDK)
for name in ('c_types.h','ip_addr.h','espconn.h'):(OUT/name).write_text('#include "esp8266.h"\n')
COMMON=r'''
#include <esp8266.h>
#include "cmd.h"
#define DBG(...) ((void)0)
bool cmdInSync=true;
static unsigned responses,responseArgs,responseValue;static uint8_t responseData[8][2048];static unsigned responseLengths[8],responseAt;
void cmdResponseStart(uint16_t cmd,uint32_t value,uint16_t argc){(void)cmd;responses++;responseValue=value;responseArgs=argc;responseAt=0;memset(responseLengths,0,sizeof responseLengths);}
void cmdResponseBody(const void *p,uint16_t n){assert(responseAt<8&&n<2048);memcpy(responseData[responseAt],p,n);responseLengths[responseAt++]=n;}
void cmdResponseEnd(void){}
'''
cmd=(ROOT/'src/cmd/cmd.c').read_text();COMMON+=cmd[cmd.index('// Execute a parsed command'):]
COMMON+=r'''
typedef struct {const void *data;unsigned n;} Arg;
static void request(unsigned command,unsigned value,unsigned argc,const Arg *args){uint8_t frame[4096]={0};CmdPacket *p=(CmdPacket*)frame;p->cmd=command;p->value=value;p->argc=argc;unsigned at=sizeof(*p);
 for(unsigned i=0;i<argc;i++){unsigned n=args[i].n;assert(at+2+((n+3)&~3U)<=sizeof frame);frame[at++]=n;frame[at++]=n>>8;memcpy(frame+at,args[i].data,n);at+=(n+3)&~3U;}
 cmdParsePacket(frame,at);
}
'''

def run(name,module,test,extra=''):
    for file,directory in [('rest.c','rest'),('socket.c','socket'),('mqtt.c','mqtt'),('pktbuf.c','mqtt'),('mqtt_msg.c','mqtt')]:
        module=module.replace('#include "'+file+'"','#include "'+str(ROOT/'src'/directory/file)+'"')
    path=OUT/(name+'-test.c');path.write_text(COMMON+'\n#include "netclient.c"\n'+extra+module+test)
    exe=OUT/name
    subprocess.run(['cc','-std=c99','-g','-O1','-fno-pie','-no-pie','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-misleading-indentation','-Wno-ignored-qualifiers',*['-I'+str(p) for p in (OUT,ROOT/'src/cmd',ROOT/'src/core',ROOT/'src/mqtt',ROOT/'src/rest',ROOT/'src/socket',ROOT/'src/httpd',ROOT/'src/web-server',ROOT/'src/syslog')],str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})

run('frames','',r'''
static unsigned calls;static void handle(CmdPacket *p){CmdRequest req;cmdRequest(&req,p);uint32_t value;assert(cmdPopArg(&req,&value,4)==0);assert(value==42);assert(cmdArgLen(&req)==0);assert(cmdPopArg(&req,&value,4)==-1);calls++;}
const CmdList commands[]={{1,"test",handle},{0,0,0}};
int main(void){uint32_t value=42;Arg a={&value,4};request(1,0,1,&a);assert(calls==1);uint8_t bad[12]={1,0,1,0,0,0,0,0,4,0};cmdParsePacket(bad,8);cmdParsePacket(bad,12);assert(calls==1);request(65535,0,0,NULL);assert(calls==1&&!allocations);puts("PASS bounded SLIP frames, argument exhaustion and unknown command dispatch");}
''')
run('rest','#include "rest.c"\n',r'''
const CmdList commands[]={{20,"setup",REST_Setup},{21,"request",REST_Request},{22,"header",REST_SetHeader},{0,0,0}};
static unsigned setup(void){uint16_t port=80;uint8_t security=0;Arg a[]={{"1.2.3.4",7},{&port,2},{&security,1}};request(20,99,3,a);return responseValue;}
int main(void){
 const char *message="HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello";
 for(unsigned split=1;split<strlen(message);split++){RestReply r={0};assert(restFeed(&r,message,split)==0);assert(restFeed(&r,message+split,strlen(message)-split)==1);assert(r.status==200&&r.bodyUsed==5&&!memcmp(r.body,"hello",5));}
 RestReply r={0};const char *chunk="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2;x=abc\r\nhe\r\n3\r\nllo\r\n0\r\nX-Trailer: v\r\n\r\n";
 for(unsigned i=0;i<strlen(chunk);i++)assert(restFeed(&r,chunk+i,1)==(i==strlen(chunk)-1?1:0));assert(r.bodyUsed==5&&!memcmp(r.body,"hello",5));
 unsigned slot=setup();assert(slot==0);Arg a[]={{"GET",3},{"/",1}};request(21,slot,2,a);RestClient *c=restClient+slot;assert(!strcmp(c->user_agent,"esp-serial2wifi"));struct espconn *p=c->pCon;connected(p);unsigned old=responses;
 p->recv_cb(p,(char*)message,20);assert(responses==old&&!p->closing);request(21,slot,2,a);assert(c->data&&c->busy);ack(p);assert(!c->data);
 p->recv_cb(p,(char*)message+20,strlen(message)-20);assert(p->closing&&responseLengths[1]==5&&!memcmp(responseData[1],"hello",5));terminal(p,false);assert(!c->busy);restFree(c);assert(!allocations);
 // Setup allocation failures preserve all existing slots and leak nothing.
 for(unsigned fail=1;fail<=7;fail++){allocateAt=allocationCalls+fail;restNum=0;setup();allocateAt=0;for(unsigned i=0;i<MAX_REST;i++)restFree(restClient+i);assert(!allocations);}
 // Request allocation failure reports 503 and preserves the configured slot.
 restNum=0;setup();for(unsigned fail=1;fail<=2;fail++){allocateAt=allocationCalls+fail;request(21,0,2,a);allocateAt=0;assert(*(int16_t*)responseData[0]==503&&!restClient[0].busy&&!restClient[0].data&&!restClient[0].reply);}restFree(restClient);assert(!allocations);
 // DNS owner remains busy until completion; cached DNS connects immediately.
 uint16_t port=80;uint8_t security=0;Arg b[]={{"test",4},{&port,2},{&security,1}};restNum=0;request(20,1,3,b);request(21,0,2,a);assert(restClient[0].resolving);unsigned first=dnsCount-1;
 restNum=0;request(20,1,3,b);assert(responseValue==UINT32_MAX);resolved(first,false);assert(!restClient[0].busy);restFree(restClient);assert(!allocations);
 puts("PASS real REST: every header/body split, chunked/trailers, single response, busy reuse, DNS and allocation failures");}
''')
run('socket','#include "socket.c"\n',r'''
const CmdList commands[]={{40,"setup",SOCKET_Setup},{41,"send",SOCKET_Send},{0,0,0}};
static unsigned setup(unsigned mode,const char *host){uint16_t port=1234;uint8_t m=mode;Arg a[]={{host,strlen(host)},{&port,2},{&m,1}};request(40,77,3,a);return responseValue;}
int main(void){
 assert(setup(SOCKET_TCP_CLIENT,"1.2.3.4")==0);SocketClient *c=socketClient;uint8_t bytes[3500];for(unsigned i=0;i<sizeof bytes;i++)bytes[i]=i;Arg a={bytes,sizeof bytes};request(41,0,1,&a);struct espconn *p=c->pCon;connected(p);
 unsigned prior=sends;ack(p);assert(sends==prior+1);while(p->borrowed)ack(p);assert(wireUsed==sizeof bytes&&!memcmp(wire,bytes,sizeof bytes)&&!c->data&&p->closing);terminal(p,false);socketFree(c);assert(!allocations);
 // Ambiguous send error retains the borrowed buffer through reset.
 socketNum=0;setup(SOCKET_TCP_CLIENT,"1.2.3.4");request(41,0,1,&a);p=c->pCon;sendError=-1;connected(p);assert(c->data&&p->borrowed&&p->closing);sendError=0;terminal(p,true);assert(!c->data);socketFree(c);assert(!allocations);
 // Broadcast payload is retained until all participating peers finish.
 socketNum=0;setup(SOCKET_TCP_SERVER,"");struct espconn peers[2]={0};for(unsigned i=0;i<2;i++){peers[i].reverse=c;peers[i].connect_cb=c->pCon->connect_cb;connected(&peers[i]);}
 request(41,0,1,&a);while(peers[0].borrowed)ack(&peers[0]);assert(c->data);while(peers[1].borrowed)ack(&peers[1]);assert(!c->data);terminal(&peers[0],false);terminal(&peers[1],false);c->listening=false;socketFree(c);assert(!allocations);
 for(unsigned fail=1;fail<=3;fail++){allocateAt=allocationCalls+fail;socketNum=0;setup(SOCKET_UDP,"1.2.3.4");allocateAt=0;for(unsigned i=0;i<MAX_SOCKET;i++)socketFree(socketClient+i);assert(!allocations);}
 socketNum=0;setup(SOCKET_UDP,"1.2.3.4");p=c->pCon;for(unsigned fail=1;fail<=3;fail++){socketNum=0;allocateAt=allocationCalls+fail;setup(SOCKET_UDP,"1.2.3.4");allocateAt=0;assert(c->pCon==p&&p->registered&&c->connected);}socketFree(c);assert(!allocations);
 puts("PASS real sockets: exact multichunk payload, no duplicate submissions, borrowed-error lifetime, broadcast ACK ownership and allocation failures");}
''')
run('mqtt','#include "pktbuf.c"\n#include "mqtt_msg.c"\n#include "mqtt.c"\n',r'''
const CmdList commands[]={{0,0,0}};
int main(void){
 MQTT_Client c={0};for(unsigned fail=1;fail<=6;fail++){allocateAt=allocationCalls+fail;assert(!MQTT_Init(&c,"test",1883,0,2,"id","user","pw",300));allocateAt=0;assert(!allocations);}
 assert(MQTT_Init(&c,"test",1883,0,255,"id","user","pw",300));assert(c.connect_info.keepalive==300);c.connect_info.clean_session=0;
 MQTT_Connect(&c);struct espconn *old=c.pCon;unsigned query=dnsCount-1;MQTT_Free(&c);assert(allocations==1);resolved(query,true);assert(!allocations);
 assert(MQTT_Init(&c,"1.2.3.4",1883,0,255,"id","user","pw",65535));c.connect_info.clean_session=0;MQTT_Connect(&c);old=c.pCon;connected(old);assert(c.keepAliveTick==65536);assert(old->borrowed);ack(old);
 uint8_t connack[]={0x20,2,0,0};for(unsigned i=0;i<sizeof connack;i++)old->recv_cb(old,(char*)connack+i,1);assert(c.connack_received&&c.connack_code==0);
 assert(MQTT_Publish(&c,"topic","bytes",5,1,0));assert(old->borrowed);
 uint16_t id=mqtt_get_id(old->borrowed,old->borrowed_length);uint8_t puback[]={0x40,2,id>>8,id};
 old->recv_cb(old,(char*)puback,sizeof puback);assert(!c.pending_buffer&&old->borrowed);ack(old);
 assert(MQTT_Subscribe(&c,"topic",2));assert(old->borrowed);uint8_t *packet=(uint8_t*)old->borrowed;assert(packet[old->borrowed_length-1]==2);ack(old);
 // Detach before reset callback; all pending/queued/LWT/callback memory retires safely.
 assert(MQTT_InitLWT(&c,"will","payload",1,0));char *will=c.connect_info.will_topic;allocateAt=allocationCalls+1;assert(!MQTT_InitLWT(&c,"next","message",1,0));allocateAt=0;assert(c.connect_info.will_topic==will);c.user_data=os_malloc(16);MQTT_Free(&c);terminal(old,true);assert(!allocations);
 assert(MQTT_Init(&c,"1.2.3.4",1883,0,2,"id","u","p",256));MQTT_Connect(&c);old=c.pCon;connected(old);
 // Settings replacement while the SDK still borrows CONNECT must preserve those bytes.
 unsigned length=old->borrowed_length;uint8_t copy[512];memcpy(copy,old->borrowed,length);MQTT_Free(&c);assert(!memcmp(copy,old->borrowed,length));ack(old);terminal(old,false);assert(!allocations);
 uint8_t invalid[]={0x30,0x80,0x80,0x80,0x80,0};assert(mqtt_get_total_length(invalid,6)==-1);assert(mqtt_get_total_length(invalid,2)==0);
 allocateAt=allocationCalls+1;assert(!PktBuf_New(128));allocateAt=0;
 puts("PASS real MQTT: allocation rollback, full-width keepalive, QoS, fragmented CONNACK, DNS retirement and borrowed buffers across settings/reset");}
''')
# Focused real web serializer and readback handlers share the actual JSON/form implementation.
HTTP=r'''
#include "httpd.h"
#include "api_request.h"
#include "api_json.c"
#include "api_request.c"
#include "operation.c"
static int code;static unsigned outputUsed;static char httpOutput[2601],guard[8]="1";
void noCacheHeaders(HttpdConnData *c,int n){(void)c;code=n;outputUsed=0;httpOutput[0]=0;}
void httpdStartResponse(HttpdConnData *c,int n){noCacheHeaders(c,n);}
void httpdHeader(HttpdConnData *c,const char *a,const char *b){(void)c;(void)a;(void)b;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
int httpdSend(HttpdConnData *c,const char *p,int n){(void)c;if(n<0)n=strlen(p);if(outputUsed+n>2600)return 0;memcpy(httpOutput+outputUsed,p,n);outputUsed+=n;httpOutput[outputUsed]=0;return 1;}
int httpdGetHeader(HttpdConnData *c,char *name,char *out,int n){(void)c;snprintf(out,n,"%s",!strcmp(name,"X-Serial2WiFi-Request")?guard:"application/x-www-form-urlencoded");return 1;}
'''
web=(ROOT/'src/web-server/web-server.c').read_text();web=web[web.index('static void ICACHE_FLASH_ATTR webResponse('):web.index('// this method is responsible for the MCU')]
run('web-response',HTTP+'\n#include "web-server.h"\n'+web,r'''
const CmdList commands[]={{0,0,0}};
int main(void){
 struct espconn socket={0};HttpdConnData c={.conn=&socket};uint8_t frame[2048]={0};CmdPacket *p=(CmdPacket*)frame;uint8_t *args=(uint8_t*)p->args;p->argc=1;
 unsigned n=904;args[0]=n;args[1]=n>>8;args[2]=WEB_STRING;args[3]='"';args[4]='\n';args[5]=0;memset(args+6,'"',900);
 CmdRequest req={.cmd=p,.arg_ptr=args,.end=frame+sizeof(*p)+2+((n+3)&~3U)};
 WEB_handleMCUResponse(&c,&req);assert(code==200&&outputUsed>1800&&strstr(httpOutput,"\\\"\\n"));
 // Truncated numeric values fail during the count pass, before response httpOutput.
 args[2]=WEB_INTEGER;WEB_handleMCUResponse(&c,&req);assert(code==503);
 // Negative fractions retain their sign, and control bytes are encoded as JSON.
 float f=-0.5f;n=6;args[0]=n;args[1]=0;args[2]=WEB_FLOAT;args[3]=0;memcpy(args+4,&f,4);req.end=frame+sizeof(*p)+2+8;
 WEB_handleMCUResponse(&c,&req);assert(code==200&&!strcmp(httpOutput,"{\"\":-0.500}"));
 assert(!allocations);puts("PASS actual custom-page JSON: escaped expansion, field names/control bytes, typed length refusal and negative fractions");}
''')
mega=(ROOT/'src/core/cgimega.c').read_text();mega=mega[mega.index('typedef struct {uint32_t address;unsigned remaining;} MegaRead;'):mega.index('void ICACHE_FLASH_ATTR sendReadFuseQuery')]
run('mega-read',HTTP+r'''
static ETSTimer optibootTimer;static HttpdConnData *readOwner;static unsigned uartCalls;
static void megaUartRecv(char *p,short n){(void)p;(void)n;}
static void (*programmingCB)(char*,short)=megaUartRecv;
static struct {void *body;} pbuf={.body=&pbuf};static unsigned progState=1;
#define stateProg 1
static void *optibootData;
static void sendLoadAddressQuery(uint32_t address){(void)address;uartCalls++;}
static bool reply_ok;
static void readLoadAddressReply(void){reply_ok=true;}
static void sendReadFlashQuery(unsigned count){assert(count<=16);uartCalls++;}
static void readReadFlashReply(char *p,unsigned count){memset(p,0x55,count);reply_ok=true;}
'''+mega,r'''
const CmdList commands[]={{0,0,0}};
int main(void){struct espconn socket={0};HttpdConnData c={.conn=&socket,.requestType=HTTPD_METHOD_POST,.url="/pgmmega/read/0,11"};assert(deviceOperationBegin(&optibootTimer));
 strcpy(guard,"");cgiMegaRead(&c);assert(code==415&&!uartCalls);strcpy(guard,"1");c.requestType=HTTPD_METHOD_GET;cgiMegaRead(&c);assert(code==405&&!uartCalls);c.requestType=HTTPD_METHOD_POST;
 c.url="/pgmmega/read/0,0";cgiMegaRead(&c);assert(code==422&&!uartCalls);c.url="/pgmmega/read/0,11";
 assert(cgiMegaRead(&c)==HTTPD_CGI_MORE);assert(c.cgiPrivData&&allocations==1);
 assert(cgiMegaRead(&c)==HTTPD_CGI_MORE);assert(cgiMegaRead(&c)==HTTPD_CGI_DONE);assert(outputUsed==73&&!allocations&&!c.cgiPrivData&&!readOwner);
 assert(cgiMegaRead(&c)==HTTPD_CGI_MORE);unsigned prior=uartCalls;c.conn=NULL;cgiMegaRead(&c);assert(!allocations&&!readOwner&&uartCalls==prior);deviceOperationEnd(&optibootTimer);
 puts("PASS actual Mega readback: mutation guards, zero/partial ranges, exact bounded rows and abort cleanup");}
''')
syslog=(ROOT/'src/syslog/syslog.c').read_text()
sent=syslog[syslog.index('static void ICACHE_FLASH_ATTR syslog_udp_sent_cb(void *arg)\n{'):syslog.index('static void ICACHE_FLASH_ATTR\nsyslog_udp_send_event')]
dns=syslog[syslog.index('static void ICACHE_FLASH_ATTR syslog_gethostbyname_cb('):syslog.index(' /******************************************************************************\n  * FunctionName : initSyslog')]
init=syslog[syslog.index('void ICACHE_FLASH_ATTR syslog_init('):syslog.index('/******************************************************************************\n * FunctionName : syslog_add_entry')]
run('syslog-config',r'''
#include "config.h"
#include "syslog.h"
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) 0,0,0,0
static struct espconn *syslog_espconn;static syslog_host_t syslogHost;static syslog_entry_t *syslogQueue;
static enum syslog_state syslogState;static ETSTimer wifi_chk_timer;
static bool syslog_timer_armed,syslog_inflight,syslog_dns_pending,syslog_reconfigure;static char syslog_pending[32];static unsigned syslog_task;
FlashConfig flashConfig;
void syslog(uint8_t facility,uint8_t severity,const char *tag,const char *fmt,...){(void)facility;(void)severity;(void)tag;(void)fmt;}
static void syslog_set_status(enum syslog_state state){syslogState=state;}
static void syslog_chk_status(void){}
static void syslog_udp_send_event(void *p){(void)p;}
static unsigned register_usr_task(void (*f)(void*)){(void)f;return 0;}
#define syslog_send_udp() ((void)0)
'''+sent+dns+init,r'''
const CmdList commands[]={{0,0,0}};
int main(void){syslog_init(NULL);syslog_init("");assert(!allocations);
 syslog_init("1.2.3.4:5514");assert(syslogHost.port==5514&&allocations==2);syslog_init("1.2.3.4");assert(syslogHost.port==514&&allocations==2);
 struct espconn *old=syslog_espconn;for(unsigned fail=1;fail<=2;fail++){allocateAt=allocationCalls+fail;syslog_init("1.2.3.4:5515");allocateAt=0;assert(syslog_espconn==old&&syslogHost.port==514&&allocations==2);}
 // Reconfiguration cannot free the connection/datagram while UDP still borrows it.
 syslogQueue=os_zalloc(sizeof(*syslogQueue)+4);syslog_inflight=true;syslog_init("");assert(syslog_reconfigure&&syslog_espconn==old&&allocations==3);
 syslog_udp_sent_cb(old);assert(!syslog_espconn&&!syslogQueue&&!allocations&&syslogState==SYSLOG_HALTED);
 syslog_init("test:5514");assert(syslog_dns_pending);unsigned query=dnsCount-1;syslog_init("");assert(syslog_reconfigure&&allocations==2);resolved(query,false);assert(!allocations&&!syslog_espconn);
 puts("PASS actual syslog configuration: default-port reset, null/empty disable, allocation rollback, deferred send/DNS cleanup");}
''')
# Complete multipart parser + actual upload-owner wrapper, with flash writes represented by callbacks.
page=(ROOT/'src/core/cgiwebserversetup.c').read_text();page=page[page.index('static MultipartCtx *webServerContext;'):page.rindex('#endif')]
run('page-owner',HTTP+r'''
#include "multipart.h"
#include "multipart.c"
static unsigned serialClients,files,bytes;
static void (*programmingCB)(char*,short);
static int serbridgeInMCUFlashing(void){return 0;}
static bool serialInputPending(void){return false;}
static unsigned serbridge_client_count(void){return serialClients;}
void errorResponse(HttpdConnData *c,int n,char *message){(void)message;noCacheHeaders(c,n);}
static int webServerSetupMultipartCallback(MultipartCmd cmd,char *data,int n,int position){(void)data;(void)position;if(cmd==FILE_START)files++;if(cmd==FILE_DATA)bytes+=n;return 0;}
'''+page,r'''
const CmdList commands[]={{0,0,0}};
int main(void){struct espconn socket={0};HttpdPostData p={0};HttpdConnData c={.conn=&socket,.post=&p,.requestType=HTTPD_METHOD_POST};
 char body[]="--boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x.txt\"\r\n\r\nhello\r\n--boundary--\r\n";
 p.multipartBoundary="--boundary";p.buff=body;p.buffLen=40;p.len=strlen(body);p.received=40;
 assert(cgiWebServerSetupUpload(&c)==HTTPD_CGI_MORE&&webServerUploadBusy()&&deviceOperationOwned(&c));
 HttpdConnData other=c;cgiWebServerSetupUpload(&other);assert(code==409&&webServerUploadBusy());
 // Conflicting activity appearing between chunks cannot bypass owner cleanup.
 serialClients=1;p.buff=body+40;p.buffLen=p.len-40;p.received=p.len;
 assert(cgiWebServerSetupUpload(&c)==HTTPD_CGI_DONE&&code==204&&!webServerUploadBusy()&&!deviceOperationBusy()&&files==1&&bytes==5&&!allocations);
 serialClients=0;p.buff=body;p.buffLen=p.received=40;assert(cgiWebServerSetupUpload(&c)==HTTPD_CGI_MORE);c.conn=NULL;cgiWebServerSetupUpload(&c);assert(!deviceOperationBusy()&&!webServerUploadBusy()&&!allocations);
 puts("PASS actual multipart upload: exclusive owner, interleaved serial activity, concurrent rejection and completion/abort cleanup");}
''')
