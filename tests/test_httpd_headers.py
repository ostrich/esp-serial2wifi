#!/usr/bin/env python3
"""Actual HTTP header/receive code: case folding and bounded request framing."""
import _paths
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/httpd/httpd.c').read_text()
lookup = source[source.index('static bool ICACHE_FLASH_ATTR httpdHeaderName('):source.index('//Setup an output buffer')]
append = source[source.index('static bool ICACHE_FLASH_ATTR httpdAppendHeader('):source.index('//Stupid li\'l helper')]
parse = source[source.index('static void ICACHE_FLASH_ATTR httpdRejectHeader('):source.index('//Callback called when there\'s data')]
receive = source[source.index('static void ICACHE_FLASH_ATTR httpdRecvCb('):source.index('static void ICACHE_FLASH_ATTR httpdDisconCb(')]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ICACHE_FLASH_ATTR
#define MAX_HEAD_LEN 1024
#define MAX_SENDBUFF_LEN 2600
#define MAX_POST 1024
#define HTTPD_METHOD_GET 1
#define HTTPD_METHOD_POST 2
#define os_strncmp strncmp
#define os_strstr strstr
typedef struct {char *head;short headPos;} Priv;
typedef struct {int len,buffSize,buffLen,received;char *buff,*multipartBoundary;} Post;
struct espconn {void *reverse;};
typedef struct {Priv *priv;Post *post;struct espconn *conn;char requestType;char *url,*getArgs;} HttpdConnData;
static unsigned allocations,disconnects,processed;static bool fail_alloc,keep_response_open;
static char consumed[8192];static unsigned consumed_length;
static void *os_malloc(unsigned n){assert(n>0&&n<=1025);if(fail_alloc)return NULL;allocations++;return malloc(n);}
static void espconn_disconnect(struct espconn *c){(void)c;disconnects++;}
static void debugConn(void *c,const char *name){(void)c;(void)name;}
static void httpdSetOutputBuffer(HttpdConnData *c,char *data,unsigned n){(void)c;(void)data;(void)n;}
static void httpdProcessRequest(HttpdConnData *c){
 processed++;assert(c->post->buffLen>=0);
 assert(consumed_length+c->post->buffLen<sizeof consumed);
 if(c->post->buffLen){memcpy(consumed+consumed_length,c->post->buff,c->post->buffLen);consumed_length+=c->post->buffLen;}
 consumed[consumed_length]=0;
 if(c->post->received==c->post->len&&!keep_response_open)c->post->len=0;
}
'''
tests = r'''
static Priv priv;static Post post;static struct espconn socket;
static char header[1025];static HttpdConnData connection={.priv=&priv,.post=&post,.conn=&socket};
static void reset(void){
 if(post.buff){free(post.buff);allocations--;}
 memset(&post,0,sizeof post);post.len=-1;memset(header,0,sizeof header);priv.head=header;priv.headPos=0;
 disconnects=processed=consumed_length=0;fail_alloc=keep_response_open=false;socket.reverse=&connection;
}
static void feed(const char *data,bool split){
 unsigned n=strlen(data);
 if(split){for(unsigned i=0;i<n;i++)httpdRecvCb(&socket,(char*)data+i,1);}
 else httpdRecvCb(&socket,(char*)data,n);
}
int main(void){
 const char *valid="POST /api/v1/test HTTP/1.1\r\ncontent-length:\t7 \t\r\nCONTENT-TYPE:\tapplication/json\r\naCcEpT-eNcOdInG: gzip\r\n\r\n{\"a\":1}";
 for(unsigned split=0;split<2;split++){
  reset();feed(valid,split);assert(!disconnects&&processed==1&&!strcmp(consumed,"{\"a\":1}"));
  char value[64];assert(httpdGetHeader(&connection,"Accept-Encoding",value,sizeof value)&&!strcmp(value,"gzip"));
  assert(httpdGetHeader(&connection,"content-type",value,sizeof value)&&!strcmp(value,"application/json"));
  strcpy(value,"must be cleared");assert(!httpdGetHeader(&connection,"Missing",value,sizeof value)&&!value[0]);
  assert(httpdGetHeader(&connection,"Content-Type",value,2)&&!strcmp(value,"a"));
  value[0]='!';assert(!httpdGetHeader(&connection,"Content-Type",value,0)&&value[0]=='!');
  feed("ignored trailing bytes",false);assert(processed==1);
 }
 const char *bad[]={"-1","+1","","2147483648","99999999999999999999","3junk","1 2","0x10","1,1"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){
  char request[256];snprintf(request,sizeof request,"POST /api/v1/test HTTP/1.1\r\nContent-Length: %s\r\n\r\nBAD",bad[i]);
  reset();feed(request,false);assert(disconnects==1&&!processed&&!allocations&&post.len==-2);
  feed("Content-Length: 3\r\n\r\nBAD",false);assert(disconnects==1&&!processed&&!allocations);
 }
 reset();feed("POST /x HTTP/1.1\r\nContent-Length: 3\r\ncOnTeNt-LeNgTh: 3\r\n\r\nBAD",false);
 assert(disconnects==1&&!processed&&allocations==1);
 reset();feed("POST /x HTTP/1.1\r\ntransfer-encoding: chunked\r\nContent-Length: 3\r\n\r\nBAD",false);
 assert(disconnects==1&&!processed&&!allocations);
 reset();fail_alloc=true;feed(valid,false);assert(disconnects==1&&!processed&&!allocations);
 reset();feed("POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n",false);assert(processed==1&&!disconnects&&post.buff[0]==0);
 reset();keep_response_open=true;feed(valid,false);feed("unexpected extra body",false);
 assert(processed==1&&post.received==7&&consumed_length==7);
 reset();char too_long[1200];memset(too_long,'x',sizeof too_long-1);too_long[sizeof too_long-1]=0;
 feed(too_long,false);feed(too_long,false);assert(disconnects==1&&!processed&&post.len==-2);
 reset();feed("POST /x HTTP/1.1\r\ncontent-length: 4096\r\n\r\n",false);
 char data[4097];memset(data,'z',4096);data[4096]=0;feed(data,false);
 assert(processed==4&&consumed_length==4096&&!memcmp(consumed,data,4096)&&!disconnects);
 reset();assert(!allocations);
 puts("PASS actual HTTP header/receive: case-insensitive names, OWS, empty/bounded lookup, fragmented and streaming bodies; negative/overflow/duplicate/chunked framing and OOM fail closed");
}
'''
build = root / '.build-tests'
build.mkdir(exist_ok=True)
path = build / 'test-httpd-headers.c'
path.write_text(prefix + lookup + append + parse + receive + tests)
binary = path.with_suffix('')
subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(path), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
