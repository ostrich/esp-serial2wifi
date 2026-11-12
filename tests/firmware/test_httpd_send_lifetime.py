#!/usr/bin/env python3
"""Actual HTTP connection/flush code must survive reuse of callback stack data."""

import _paths
from source_text import SourceText
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
s = SourceText((root / "src/httpd/httpd.c").read_text())
connect = s[
    s.index("static void ICACHE_FLASH_ATTR httpdConnectCb(") : s.index(
        "//Httpd initialization routine."
    )
]
release = s[
    s.index("static void ICACHE_FLASH_ATTR httpdReleaseSend(") : s.index(
        "static void ICACHE_FLASH_ATTR httpdRetireConn("
    )
]
flush = s[
    s.index("void ICACHE_FLASH_ATTR httpdFlush(") : s.index("// Cooperative HTTP read scheduling")
]
fileSource = SourceText((root / "src/httpd/httpdespfs.c").read_text())
fileCgi = fileSource[fileSource.index("int ICACHE_FLASH_ATTR") : fileSource.index("#if 0")]
work = s[
    s.index("// Cooperative HTTP read scheduling") : s.index(
        "//Callback called when the data on a socket has been successfully sent."
    )
]
sent = s[
    s.index("static void ICACHE_FLASH_ATTR httpdSentCb(") : s.index(
        "static const char *httpNotFoundHeader"
    )
]
process = s[
    s.index("static const char *httpNotFoundHeader") : s.index("//Parse a line of header data")
]
retire = s[
    s.index("static void ICACHE_FLASH_ATTR httpdRetireConn(") : s.index(
        "//Stupid li", s.index("static void ICACHE_FLASH_ATTR httpdRetireConn(")
    )
]
prelude = r"""
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define ICACHE_FLASH_ATTR
#define HTTPDVER "test"
#define FLAG_GZIP 2
#define MAX_URL_LEN 255
#define MAX_CONN 6
#define MAX_SENDBUFF_LEN 2600
#define HTTPD_CGI_MORE 0
#define HTTPD_CGI_DONE 1
#define HTTPD_CGI_NOTFOUND 2
#define HTTPD_CGI_AUTHENTICATED 3
#define MAX_HEAD_LEN 1024
#define HTTPD_METHOD_GET 1
#define HTTPD_METHOD_POST 2
#define os_strcmp strcmp
#define os_strncmp strncmp
#define ESPCONN_COPY 4
#define ESPCONN_REUSEADDR 1
#define ESPCONN_NODELAY 2
#define DBG(...) ((void)0)
#define os_printf printf
#define os_sprintf sprintf
#define os_memcpy memcpy
#define os_strlen strlen
#define os_strstr strstr
static unsigned allocations,fail_alloc,disconnects;
static void *os_malloc(unsigned n){if(fail_alloc)return NULL;void *p=malloc(n);assert(p);allocations++;return p;}
static void os_free(void *p){assert(allocations);allocations--;free(p);}
#define espconn_regist_recvcb(c,cb) ((void)(c))
#define espconn_regist_reconcb(c,cb) ((void)(c))
#define espconn_regist_disconcb(c,cb) ((void)(c))
#define espconn_regist_sentcb(c,cb) ((void)(c))
typedef int8_t sint8;
typedef uint32_t uint32;
typedef struct {unsigned char remote_ip[4];unsigned remote_port;} esp_tcp;
struct espconn {void *reverse;struct {esp_tcp *tcp;} proto;unsigned opts;};
typedef struct {int headPos,sendBuffLen,sendBuffMax;char from[64];char *sendBuff,*pendingSend,*head;bool sendFailed,readDeferred;uint32_t readNotBefore;unsigned short cgiIndex;} Priv;
typedef struct {char *buff,*multipartBoundary;int buffLen,received,len;} Post;
typedef struct HttpdConnData {Priv *priv;struct espconn *conn;Post *post;unsigned startTime;char *url,*getArgs;void *cgiPrivData,*cgiData,*cgiArg,*cgiResponse;unsigned requestType;int (*cgi)(struct HttpdConnData*);} HttpdConnData;
static void httpdSetOutputBuffer(HttpdConnData *c,char *b,int n){c->priv->sendBuffMax=n;c->priv->sendBuff=b;c->priv->sendBuffLen=0;}
static HttpdConnData connData[MAX_CONN];static Priv connPrivData[MAX_CONN];static Post connPostData[MAX_CONN];
static const char *connStr="";
static void debugConn(void *c,const char *s){(void)c;(void)s;}
typedef struct {void (*fn)(void *);void *arg;bool armed;} ETSTimer;
static void os_timer_setfn(ETSTimer *t,void (*fn)(void *),void *arg){t->fn=fn;t->arg=arg;}
static void os_timer_arm(ETSTimer *t,unsigned ms,bool repeat){assert(ms==10&&!repeat);t->armed=true;}
static unsigned serialPending,livePending,activeClients,heap=24000;
static unsigned consoleStreamPending(void){return livePending;}
int apiSerialLive(HttpdConnData *c){(void)c;return HTTPD_CGI_MORE;}
static uint8_t serbridge_client_count(void){return activeClients;}
static uint32_t serbridgePendingTx(void){return serialPending;}
static unsigned system_get_free_heap_size(void){return heap;}
static unsigned now=1;
static unsigned system_get_time(void){return now;}
int cgiEspFsHook(HttpdConnData *);
int cgiUserFsHook(HttpdConnData *);
static int reply(HttpdConnData *);
typedef struct {char *url;int (*cgiCb)(HttpdConnData *);void *cgiArg;} HttpdBuiltInUrl;
static HttpdBuiltInUrl routes[]={{"/api/read",reply,NULL},{"/asset.js",cgiEspFsHook,NULL},{NULL,NULL,NULL}};
static HttpdBuiltInUrl *builtInUrls=routes;
static void espconn_disconnect(struct espconn *c){(void)c;disconnects++;}
static void espconn_set_opt(struct espconn *c,unsigned opts){c->opts|=opts;}
static const uint8_t *pending;static unsigned pending_len,reject_send;
// Model worst-case deferred SDK reads, including partial acceptance on error.
static sint8 espconn_sent(struct espconn *c,uint8_t *p,unsigned n){
 (void)c;pending_len=n;pending=p;return reject_send?-1:0;
}
"""
prelude = prelude.replace(
    "#define MAX_URL_LEN 255",
    next(line for line in fileSource.splitlines() if line.startswith("#define MAX_URL_LEN ")),
)
fsPrelude = r"""
static unsigned filePos,fileLength=4096+37,fileCloses;static bool missingFile,userOnly;
typedef struct {int dummy;} EspFsFile;
static void *lastOpened;static EspFsFile fakeFile;static void *firmwareFsCtx=(void*)1,*userPageCtx=(void*)2;
static const char *gzipNonSupportedMessage="gzip required";
static EspFsFile *espFsOpen(void *ctx,char *name){lastOpened=ctx;assert(name);if(missingFile||(userOnly&&ctx==firmwareFsCtx))return NULL;filePos=0;return &fakeFile;}
static int espFsIsValid(void *ctx){return ctx==userPageCtx;}
static int espFsFlags(EspFsFile *file){(void)file;return 0;}
static int espFsRead(EspFsFile *file,char *out,int size){assert(file==&fakeFile);unsigned n=fileLength-filePos;if(n>(unsigned)size)n=size;for(unsigned i=0;i<n;i++)out[i]=(char)((filePos+i)%251);filePos+=n;return n;}
static void espFsClose(EspFsFile *file){if(file)fileCloses++;}
static int httpdUrlDecode(char *a,int n,char *b,int max){assert(n<max);memcpy(b,a,n);b[n]=0;return n;}
static int httpdGetHeader(HttpdConnData *c,char *name,char *out,int n){(void)c;(void)name;snprintf(out,n,"gzip");return 1;}
static void httpdStartResponse(HttpdConnData *c,int code){(void)c;(void)code;}
static void httpdHeader(HttpdConnData *c,const char *name,const char *value){(void)c;(void)name;(void)value;}
static void httpdEndHeaders(HttpdConnData *c){(void)c;}
static const char *httpdGetMimetype(char *url){(void)url;return "application/octet-stream";}
static int httpdSend(HttpdConnData *c,const char *data,int n){if(n<0)n=strlen(data);assert(c->priv->sendBuffLen+n<=c->priv->sendBuffMax);memcpy(c->priv->sendBuff+c->priv->sendBuffLen,data,n);c->priv->sendBuffLen+=n;return 1;}
"""
tests = r"""
static unsigned replies;
static int reply(HttpdConnData *c){if(!c->conn)return HTTPD_CGI_DONE;replies++;httpdSend(c,"response",8);return HTTPD_CGI_DONE;}
static void timerTick(void){assert(httpdReadTimer.armed);httpdReadTimer.armed=false;httpdReadTimer.fn(httpdReadTimer.arg);}

static int cleanup(HttpdConnData *c){assert(c->priv->head&&c->url==c->priv->head);return 1;}
int main(void){
 esp_tcp tcp={{192,168,4,2},12345};struct espconn sdk={0};sdk.proto.tcp=&tcp;
 fail_alloc=1;httpdConnectCb(&sdk);assert(!sdk.reverse&&!allocations&&disconnects==1);fail_alloc=0;disconnects=0;
 connData[0].cgiPrivData=(void*)1;
 httpdConnectCb(&sdk);HttpdConnData *c=sdk.reverse;assert(c&&!c->cgiPrivData);
 unsigned baseline=allocations;assert(baseline==1&&c->priv->head&&!c->priv->head[0]);
 char stack_output[2600];
 for(unsigned n=1;n<=sizeof stack_output;n=n==1?536:n==536?2600:2601){
  for(unsigned i=0;i<n;i++)stack_output[i]=(char)(i%251);
  c->priv->sendBuff=stack_output;c->priv->sendBuffLen=n;httpdFlush(c);
  assert(!c->priv->sendBuffLen&&pending_len==n);
  memset(stack_output,0xa5,sizeof stack_output);
  for(unsigned i=0;i<n;i++)assert(pending[i]==i%251);
  pending=NULL;httpdReleaseSend(c);assert(allocations==baseline&&!c->priv->pendingSend);
 }
 fail_alloc=1;c->priv->sendBuffLen=20;httpdFlush(c);assert(disconnects==1&&allocations==baseline&&c->priv->sendFailed);fail_alloc=0;
 httpdRetireConn(c);httpdConnectCb(&sdk);c=sdk.reverse;assert(!c->priv->sendFailed);c->priv->sendBuff=stack_output;
 reject_send=1;c->priv->sendBuffLen=20;httpdFlush(c);assert(disconnects==2&&allocations==baseline+1&&pending==(uint8_t*)c->priv->pendingSend);
 memset(stack_output,0,sizeof stack_output);assert(pending[0]==0xa5);
 httpdSentCb(&sdk);assert(allocations==baseline+1&&pending[0]==0xa5&&c->priv->pendingSend);
 // Even another callback cannot free the source of an ambiguously accepted record.
 httpdSentCb(&sdk);assert(allocations==baseline+1&&pending[0]==0xa5);
 pending=NULL;httpdRetireConn(c);assert(!allocations);httpdConnectCb(&sdk);c=sdk.reverse;c->priv->sendBuff=stack_output;
 reject_send=0;c->priv->sendBuffLen=20;httpdFlush(c);const uint8_t *first=pending;
 c->priv->sendBuffLen=20;httpdFlush(c);assert(disconnects==3&&pending==first&&allocations==baseline+1);
 httpdSentCb(&sdk);assert(allocations==baseline+1&&pending==first);
 pending=NULL;httpdRetireConn(c);httpdConnectCb(&sdk);c=sdk.reverse;assert(!c->priv->sendFailed);
 strcpy(c->priv->head,"GET /asset.js");c->priv->headPos=50;c->url=c->priv->head+4;c->getArgs=c->priv->head+6;c->cgiData=NULL;
 c->priv->sendBuff=stack_output;c->priv->sendBuffMax=sizeof(stack_output);c->priv->sendBuffLen=0;
 assert(cgiEspFsHook(c)==0&&c->priv->headPos==-1&&!strcmp(c->url,"/asset.js")&&!c->getArgs);
 unsigned received=0;
 while(received<fileLength){
  c->priv->sendBuff=stack_output;c->priv->sendBuffLen=0;
  int result=cgiEspFsHook(c);unsigned n=c->priv->sendBuffLen;assert(n>0&&n<=512);
  httpdFlush(c);memset(stack_output,0xa5,sizeof(stack_output));
  for(unsigned i=0;i<n;i++)assert(pending[i]==(received+i)%251);
  received+=n;assert(result==(received==fileLength));pending=NULL;httpdReleaseSend(c);
 }
 assert(fileCloses==1&&!c->cgiData);httpdRetireConn(c);httpdConnectCb(&sdk);c=sdk.reverse;
 for(unsigned i=0;i<MAX_HEAD_LEN;i++)assert(httpdAppendHeader(c,'x'));
 assert(c->priv->headPos==MAX_HEAD_LEN&&!c->priv->head[MAX_HEAD_LEN]);assert(!httpdAppendHeader(c,'x'));
 c->url=c->priv->head;c->getArgs=c->priv->head+1;c->cgi=cleanup;httpdRetireConn(c);assert(!allocations&&!c->priv->head&&!c->url&&!c->getArgs);
 // Backlogged/low-heap GETs do not produce or allocate response copies.
 httpdConnectCb(&sdk);c=sdk.reverse;c->requestType=HTTPD_METHOD_GET;
 strcpy(c->priv->head,"/api/read");c->url=c->priv->head;serialPending=5000;
 httpdSetOutputBuffer(c,stack_output,sizeof stack_output);unsigned old=allocations;httpdProcessRequest(c);
 assert(c->priv->readDeferred&&httpdReadTimer.armed&&!replies&&allocations==old);
 timerTick();assert(!replies&&c->priv->readDeferred);
 serialPending=1000;heap=6000;timerTick();assert(!replies&&c->priv->readDeferred);
 heap=16000;timerTick();assert(replies==1&&!c->priv->readDeferred&&c->priv->pendingSend&&!strcmp(c->url,"/api/read"));
 // An idle but connected bridge retains the same admission budget, avoiding
 // a response burst in the gap before the next UART task posts bytes.
 serialPending=0;activeClients=1;
 // A second read waits for ACK ownership to be released, then makes progress.
 struct espconn other={0};other.proto.tcp=&tcp;httpdConnectCb(&other);HttpdConnData *second=other.reverse;second->requestType=HTTPD_METHOD_GET;
 strcpy(second->priv->head,"/api/read");second->url=second->priv->head;httpdSetOutputBuffer(second,stack_output,sizeof stack_output);httpdProcessRequest(second);
 assert(second->priv->readDeferred&&replies==1);httpdSentCb(&sdk);httpdRetireConn(c);timerTick();assert(replies==2&&!second->priv->readDeferred&&second->priv->pendingSend);
 httpdReleaseSend(second);httpdRetireConn(second);pending=NULL;activeClients=0;
 // Deferred work cannot run on a disconnected/reused connection slot.
 httpdConnectCb(&sdk);c=sdk.reverse;c->requestType=HTTPD_METHOD_GET;strcpy(c->priv->head,"/api/read");c->url=c->priv->head;serialPending=5000;
 httpdSetOutputBuffer(c,stack_output,sizeof stack_output);httpdProcessRequest(c);assert(c->priv->readDeferred);
 httpdRetireConn(c);httpdConnectCb(&sdk);c=sdk.reverse;timerTick();assert(replies==2&&!c->priv->readDeferred);httpdRetireConn(c);
 // File opening releases headers even under pressure; data chunks yield.
 httpdConnectCb(&sdk);c=sdk.reverse;c->requestType=HTTPD_METHOD_GET;strcpy(c->priv->head,"/asset.js");c->url=c->priv->head;
 httpdSetOutputBuffer(c,stack_output,sizeof stack_output);httpdProcessRequest(c);assert(c->cgiData&&c->priv->headPos==-1&&filePos==0);
 httpdSentCb(&sdk);assert(c->priv->readDeferred&&filePos==0);
 serialPending=1000;timerTick();assert(filePos==512&&c->priv->pendingSend&&!c->priv->readDeferred);
 httpdConnectCb(&other);second=other.reverse;second->requestType=HTTPD_METHOD_GET;strcpy(second->priv->head,"/api/read");second->url=second->priv->head;
 httpdSetOutputBuffer(second,stack_output,sizeof stack_output);httpdProcessRequest(second);assert(second->priv->readDeferred);
 httpdSentCb(&sdk);assert(c->priv->readDeferred&&filePos==512);timerTick();assert(replies==3&&second->priv->pendingSend&&filePos==512);
 httpdRetireConn(second);httpdRetireConn(c);pending=NULL;
 // Timed streaming CGI waits keep no output buffer and cannot run before due
 // or while the SDK still owns the prior response. Disconnect cancels the timer.
 serialPending=0;activeClients=0;httpdConnectCb(&sdk);c=sdk.reverse;c->requestType=HTTPD_METHOD_GET;
 strcpy(c->priv->head,"/api/read");c->url=c->priv->head;c->cgi=reply;
 unsigned oldReplies=replies;httpdWait(c,30);timerTick();assert(replies==oldReplies&&c->priv->readDeferred);
 now+=30000;timerTick();assert(replies==oldReplies+1&&c->priv->pendingSend);
 httpdWait(c,30);now+=30000;timerTick();assert(replies==oldReplies+1);
 httpdRetireConn(c);pending=NULL;timerTick();assert(replies==oldReplies+1);
 serialPending=7000;activeClients=1;
 // Streaming writes are processed synchronously, never parked on a reused POST buffer.
 httpdConnectCb(&sdk);c=sdk.reverse;c->requestType=HTTPD_METHOD_POST;strcpy(c->priv->head,"/api/read");c->url=c->priv->head;serialPending=12000;heap=3000;
 httpdSetOutputBuffer(c,stack_output,sizeof stack_output);httpdProcessRequest(c);assert(replies==oldReplies+2&&!c->priv->readDeferred);httpdRetireConn(c);pending=NULL;serialPending=0;heap=24000;
 // A live FIFO backlog defers background GETs but never its own drain.
 activeClients=0;livePending=4096;httpdConnectCb(&sdk);c=sdk.reverse;c->requestType=HTTPD_METHOD_GET;c->cgi=reply;
 assert(httpdDeferRead(c)&&c->priv->readDeferred);c->priv->readDeferred=false;c->cgi=apiSerialLive;
 assert(!httpdDeferRead(c));httpdRetireConn(c);livePending=0;timerTick();
 // Missing static files return not found; real files retain ACK-owned lifetime.
 httpdConnectCb(&sdk);c=sdk.reverse;strcpy(c->priv->head,"/index.html");c->url=c->priv->head;httpdSetOutputBuffer(c,stack_output,sizeof stack_output);
 missingFile=true;assert(cgiEspFsHook(c)==HTTPD_CGI_NOTFOUND);missingFile=false;
 assert(cgiEspFsHook(c)==HTTPD_CGI_MORE&&c->cgiData);c->cgi=cgiEspFsHook;httpdRetireConn(c);
 // A 255-byte filename needs room for its leading slash and NUL when
 // the user-filesystem URL is decoded. This is the actual production limit.
 httpdConnectCb(&sdk);c=sdk.reverse;char longUrl[257];longUrl[0]='/';memset(longUrl+1,'a',255);longUrl[256]=0;
 strcpy(c->priv->head,longUrl);c->url=c->priv->head;httpdSetOutputBuffer(c,stack_output,sizeof stack_output);userOnly=true;
 assert(cgiEspFsHook(c)==HTTPD_CGI_MORE&&c->cgiData);c->cgi=cgiEspFsHook;httpdRetireConn(c);userOnly=false;
 // Namespaced index.html must bypass the built-in index and close on abort.
 httpdConnectCb(&sdk);c=sdk.reverse;strcpy(c->priv->head,"/user/index.html");c->url=c->priv->head;c->cgi=cgiUserFsHook;httpdSetOutputBuffer(c,stack_output,sizeof stack_output);userOnly=false;
 int userResult=cgiUserFsHook(c);assert(userResult==HTTPD_CGI_MORE&&c->cgiData&&lastOpened==userPageCtx&&!strcmp(c->url,"/index.html"));httpdRetireConn(c);userOnly=false;
 // All six advertised slots remain available; reject the seventh without allocating.
 struct espconn many[MAX_CONN+1];memset(many,0,sizeof many);
 for(unsigned i=0;i<MAX_CONN+1;i++){many[i].proto.tcp=&tcp;httpdConnectCb(&many[i]);}
 assert(allocations==MAX_CONN&&!many[MAX_CONN].reverse);
 for(unsigned i=0;i<MAX_CONN;i++)httpdRetireConn(many[i].reverse);
 assert(!allocations);
 puts("PASS actual HTTP ownership/scheduling: deferred reads, ACK admission, fair file/JSON turns, abort/reuse, synchronous writes, borrowed-buffer lifetime and OOM");
}
"""
out = root / ".build-tests"
out.mkdir(exist_ok=True)
src = out / "test-httpd-lifetime.c"
src.write_text(
    prelude
    + release
    + retire
    + connect
    + flush
    + work
    + sent
    + fsPrelude
    + fileCgi
    + process
    + tests
)
exe = out / "test-httpd-lifetime"
subprocess.run(
    [
        "gcc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Wno-unused-variable",
        "-Wno-sign-compare",
        "-fsanitize=address,undefined",
        str(src),
        "-o",
        str(exe),
    ],
    check=True,
)
subprocess.run(
    [str(exe)],
    check=True,
    env={**__import__("os").environ, "ASAN_OPTIONS": "detect_stack_use_after_return=1"},
)
