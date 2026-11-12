#!/usr/bin/env python3
"""Actual multipart parser: exact binary files, arbitrary splits and incomplete requests."""

import _paths
import os, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / ".build-tests/multipart"
OUT.mkdir(parents=True, exist_ok=True)
source = r"""
#include <assert.h>
#include <esp8266.h>
#define os_malloc malloc
#define os_memmove memmove
#define os_strncmp strncmp
#include "multipart.c"
static int code;static unsigned starts,ends,commits,used,fileStart;static unsigned wantFiles=1;static unsigned char result[4096];
void errorResponse(HttpdConnData *c,int n,char *message){(void)c;(void)message;code=n;}
void httpdStartResponse(HttpdConnData *c,int n){(void)c;code=n;}
void httpdEndHeaders(HttpdConnData *c){(void)c;}
static int callback(MultipartCmd cmd,char *data,int n,int position){
 switch(cmd){case FILE_UPLOAD_START:break;case FILE_START:assert(!strcmp(data,"test.bin"));starts++;fileStart=used;break;
 case FILE_DATA:assert(used+n<=sizeof result);memcpy(result+used,data,n);used+=n;break;
 case FILE_DONE:assert((unsigned)position==used-fileStart);ends++;break;case FILE_UPLOAD_DONE:commits++;break;}return 0;
}
static struct espconn socket;
static HttpdPostData post;
static HttpdConnData conn={.conn=&socket,.post=&post,.requestType=HTTPD_METHOD_POST};
static unsigned char payload[1024],message[4096];static unsigned total;
static void run(unsigned chunk,unsigned length,bool complete){
 MultipartCtx *c=multipartCreateContext(callback);assert(c);code=starts=ends=commits=used=0;
 post.len=length;post.multipartBoundary="--boundary";
 for(unsigned at=0;at<length;){unsigned n=length-at;if(n>chunk)n=chunk;post.buff=(char*)message+at;post.buffLen=n;at+=n;int r=multipartProcess(c,&conn);if(r==HTTPD_CGI_DONE)break;}
 if(complete){assert(code==204&&starts==wantFiles&&ends==wantFiles&&commits==1&&used==sizeof payload*wantFiles&&!memcmp(result,payload,sizeof payload));}
 else assert(code==400&&!commits);
 multipartDestroyContext(c);
}
int main(void){
 for(unsigned n=0;n<sizeof payload;n++)payload[n]=(unsigned char)n;
 memcpy(payload+200,"\r\n--boundaryX",13); // a delimiter prefix inside binary content
 const char *header="--boundary\r\nContent-Disposition: form-data; name=\"webpage\"; filename=\"test.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
 total=strlen(header);memcpy(message,header,total);memcpy(message+total,payload,sizeof payload);total+=sizeof payload;
 const char *tail="\r\n--boundary--\r\n";memcpy(message+total,tail,strlen(tail));total+=strlen(tail);
 unsigned chunks[]={1,2,7,17,127,128,255,256,511,512,1024};
 for(unsigned n=0;n<sizeof chunks/sizeof *chunks;n++)run(chunks[n],total,true);
 for(unsigned cut=3;cut<20;cut++)run(17,total-cut,false);
 total-=strlen(tail);
 const char *middle="\r\n--boundary\r\nContent-Disposition: form-data; name=\"webpage\"; filename=\"test.bin\"\r\n\r\n";
 memcpy(message+total,middle,strlen(middle));total+=strlen(middle);memcpy(message+total,payload,sizeof payload);total+=sizeof payload;
 const char *field="\r\n--boundary\r\nContent-Disposition: form-data; name=\"submit\"\r\n\r\nSubmit";
 memcpy(message+total,field,strlen(field));total+=strlen(field);memcpy(message+total,tail,strlen(tail));total+=strlen(tail);
 wantFiles=2;run(17,total,true);assert(!memcmp(result+sizeof payload,payload,sizeof payload));
 puts("PASS multipart: exact binary payload, split delimiters, prefix lookalikes and incomplete body refusal");
}
"""
(OUT / "test.c").write_text(source)
subprocess.run(
    [
        "cc",
        "-std=c11",
        "-g",
        "-O1",
        "-fsanitize=address,undefined",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-misleading-indentation",
        "-Wno-ignored-qualifiers",
        *["-I" + str(ROOT / p) for p in ["tests/support/api-stubs", "src/httpd", "src/core"]],
        str(OUT / "test.c"),
        "-o",
        str(OUT / "test"),
    ],
    check=True,
)
subprocess.run(
    [str(OUT / "test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"}
)
