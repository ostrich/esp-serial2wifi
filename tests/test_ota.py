#!/usr/bin/env python3
"""Fault-inject actual OTA handlers, verifier and flash guards without a device."""
import _paths
from pathlib import Path
import subprocess
import tempfile
import flash_layout
from test_flash_layout import STUB

ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / '.build-tests/ota'
PRIVATE.mkdir(parents=True, exist_ok=True)

HTTP = r'''
#pragma once
typedef struct {int len,buffLen,received;char *buff;} HttpdPostData;
typedef struct {void *conn,*cgiPrivData;HttpdPostData *post;int requestType,code;} HttpdConnData;
#define HTTPD_METHOD_POST 1
#define HTTPD_CGI_DONE 1
#define HTTPD_CGI_MORE 0
static void httpdStartResponse(HttpdConnData *c,int code) {c->code=code;}
static void httpdHeader(HttpdConnData *c,const char *name,const char *value) {(void)c;(void)name;(void)value;}
static void httpdEndHeaders(HttpdConnData *c) {(void)c;}
'''
SDK = r'''
#include <stdio.h>
#include <stdlib.h>
#define os_memset memset
#define os_zalloc test_alloc
#define os_free test_free
typedef unsigned char uint8;
typedef int ETSTimer;
typedef void os_timer_func_t(void *);
#define UPGRADE_FLAG_FINISH 2
static unsigned current_slot,upgrade_flag,scheduled,allocations,allocation_failure;
static void *test_alloc(size_t n) {if(allocation_failure)return NULL;allocations++;return calloc(1,n);}
static void test_free(void *p) {assert(allocations);allocations--;free(p);}
static uint8 system_upgrade_userbin_check(void) {return current_slot;}
static void system_upgrade_flag_set(unsigned flag) {upgrade_flag=flag;}
static void system_upgrade_reboot(void) {}
static void os_timer_disarm(ETSTimer *t) {(void)t;}
static void os_timer_setfn(ETSTimer *t,os_timer_func_t *f,void *p) {(void)t;(void)f;(void)p;}
static void os_timer_arm(ETSTimer *t,unsigned n,int repeat) {(void)t;assert(n==2000&&!repeat);scheduled++;}
'''
TEST = r'''
void (*programmingCB)(char *,short);
int serbridgeInMCUFlashing(void){return 0;}
bool serialInputPending(void){return false;}
uint8_t serbridge_client_count(void){return 0;}
bool apiWrite(HttpdConnData *c){return c->conn&&c->requestType==HTTPD_METHOD_POST;}

void errorResponse(HttpdConnData *c,int code,char *message) {(void)message;c->code=code;}
static void reset(void) {
 assert(active_upload==NULL&&allocations==0);
 verified_slot=-1;activating=false;deviceOperationEnd(&flash_reboot_timer);upgrade_flag=scheduled=0;current_slot=0;
 writes=erases=fail=fail_write=fail_read=corrupt_read=allocation_failure=0;test_map=FLASH_LAYOUT_MAP;capacity=0;while((1U<<capacity)<FLASH_LAYOUT_BYTES)capacity++;
 memset(memory,0x5a,sizeof memory);
}
static void protected_unchanged(unsigned slot) {
 unsigned start=slot?FLASH_APP2_START:FLASH_APP1_START;
 unsigned size=slot?FLASH_APP2_SIZE:FLASH_APP1_SIZE;
 for(unsigned i=0;i<sizeof memory;i++)if(i<start||i>=start+size)assert(memory[i]==0x5a);
}
static void begin(HttpdConnData *c,HttpdPostData *p,uint8_t *image,unsigned size) {
 memset(c,0,sizeof *c);c->conn=c;c->post=p;c->requestType=HTTPD_METHOD_POST;
 p->len=size;p->buff=(char *)image;p->received=p->buffLen=size<1024?size:1024;
}
static void abort_request(HttpdConnData *c) {c->conn=NULL;cgiUploadFirmware(c);assert(!active_upload&&!allocations);}
static void upload(uint8_t *image,unsigned size,unsigned slot,int expected) {
 reset();current_slot=1-slot;
 HttpdConnData c;HttpdPostData p;begin(&c,&p,image,size);
 for(unsigned offset=0;offset<size;) {
  unsigned count=size-offset;if(count>1024)count=1024;
  p.buff=(char *)image+offset;p.buffLen=count;p.received=offset+count;
  int result=cgiUploadFirmware(&c);offset+=count;
  if(result==HTTPD_CGI_DONE)break;
 }
 assert(c.code==expected);assert(!active_upload&&!allocations);
 protected_unchanged(slot);
 if(expected==200) {
  assert(!memcmp(memory+(slot?FLASH_APP2_START:FLASH_APP1_START),image,size));
  assert(verified_slot==(int)slot);cgiRebootFirmware(&c);
  assert(c.code==200&&upgrade_flag==UPGRADE_FLAG_FINISH&&scheduled==1);
 } else {assert(verified_slot==-1);cgiRebootFirmware(&c);assert(c.code==409&&!scheduled);}
 abort_request(&c);
}
static uint8_t *read_image(char *path,unsigned *size) {
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);
 uint8_t *data=malloc(*size);assert(data&&fread(data,1,*size,f)==*size);fclose(f);return data;
}
int main(int argc,char **argv) {
 assert(argc==3);unsigned size1,size2;
 uint8_t *one=read_image(argv[1],&size1),*two=read_image(argv[2],&size2);
 reset();
 if(!firmwareOTASupported()) {
  reset();HttpdConnData c;HttpdPostData p;begin(&c,&p,two,size2);
  cgiUploadFirmware(&c);assert(c.code==400&&!writes&&!erases&&!allocations);
  cgiRebootFirmware(&c);assert(c.code==400&&!scheduled);
  assert(!firmwareOTASupported());
  abort_request(&c);free(one);free(two);
  puts("PASS disabled/untested layout blocks OTA without flash writes");return 0;
 }
 upload(one,size1,0,200);upload(two,size2,1,200);
 // Once activation is accepted, neither a replacement upload nor another reset
 // may invalidate the candidate or replace the scheduled boot operation.
 unsigned sealedWrites=writes,sealedErases=erases;HttpdConnData sealed;HttpdPostData sealedPost;
 begin(&sealed,&sealedPost,two,size2);cgiUploadFirmware(&sealed);
 assert(sealed.code==409&&writes==sealedWrites&&erases==sealedErases&&verified_slot==1&&scheduled==1);
 cgiRebootFirmware(&sealed);assert(sealed.code==409&&scheduled==1);


 // Malformed first header and oversize requests must not erase anything.
 reset();HttpdConnData c;HttpdPostData p;one[0]^=1;
 begin(&c,&p,one,size1);cgiUploadFirmware(&c);
 assert(c.code==400&&!writes&&!erases);abort_request(&c);one[0]^=1;
 reset();begin(&c,&p,two,FLASH_APP2_SIZE+4);cgiUploadFirmware(&c);
 assert(c.code==400&&!writes&&!erases);abort_request(&c);
 // Concurrent uploads are rejected without releasing the first owner.
 reset();begin(&c,&p,two,size2);assert(cgiUploadFirmware(&c)==HTTPD_CGI_MORE);
 HttpdConnData other;HttpdPostData op;begin(&other,&op,two,size2);
 unsigned oldwrites=writes,olderases=erases;cgiUploadFirmware(&other);
 assert(other.code==409&&active_upload&&writes==oldwrites&&erases==olderases);
 other.conn=NULL;cgiUploadFirmware(&other);assert(active_upload);
 abort_request(&c);protected_unchanged(1);
 assert(memory[FLASH_APP2_START]==255); // Partial image made unbootable.
 // Duplicated/misordered chunk invalidates only the inactive candidate.
 reset();begin(&c,&p,two,size2);cgiUploadFirmware(&c);cgiUploadFirmware(&c);
 assert(c.code==409);abort_request(&c);protected_unchanged(1);
 // SDK errors and allocation failure must not become upload success.
 reset();fail=1;begin(&c,&p,two,size2);cgiUploadFirmware(&c);
 assert(c.code==500&&verified_slot==-1);abort_request(&c);protected_unchanged(1);
 reset();fail_write=1;begin(&c,&p,two,size2);cgiUploadFirmware(&c);
 assert(c.code==500&&verified_slot==-1);abort_request(&c);protected_unchanged(1);
 reset();fail_read=1;begin(&c,&p,two,size2);cgiUploadFirmware(&c);
 assert(c.code==500&&verified_slot==-1);abort_request(&c);protected_unchanged(1);
 reset();corrupt_read=1;begin(&c,&p,two,size2);cgiUploadFirmware(&c);
 assert(c.code==500&&verified_slot==-1);abort_request(&c);protected_unchanged(1);
 reset();allocation_failure=1;begin(&c,&p,two,size2);cgiUploadFirmware(&c);
 assert(c.code==503&&!writes&&!erases);abort_request(&c);
 // Corrupt footer is rejected, and partial inactive header is erased.
 two[size2-1]^=1;upload(two,size2,1,400);two[size2-1]^=1;
 reset();begin(&c,&p,two,size2);cgiRebootFirmware(&c);assert(c.code==409&&!scheduled);
 free(one);free(two);
 puts("PASS actual OTA: both slots, bad header/CRC/size, concurrent/aborted/misordered uploads, SDK/OOM failures, protected regions and reboot gate");
}
'''

with tempfile.TemporaryDirectory(dir=PRIVATE) as name:
    work = Path(name)
    (work / 'esp8266.h').write_text(STUB + SDK)
    (work / 'httpd.h').write_text(HTTP)
    (work / 'generated_flash_layout.h').write_text(flash_layout.header(flash_layout.layout('4MB')))
    includes = ''.join('#include "' + str(ROOT/'src/core' / file) + '"\n'
                       for file in ('operation.c','flash_layout.c', 'firmware_image.c', 'cgiflash.c'))
    (work / 'test.c').write_text(includes + TEST)
    for enabled,testing in ((0,0),(0,1),(1,0)):
        subprocess.run(['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                        '-DSERIAL2WIFI_OTA_ENABLED=' + str(enabled), '-DSERIAL2WIFI_OTA_TESTING=' + str(testing), '-I' + str(work),'-I'+str(ROOT/'tests/api-stubs'),'-I'+str(ROOT/'src/serial'),
                        str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        subprocess.run([str(work / 'test'), str(ROOT / 'firmware/user1.bin'),
                        str(ROOT / 'firmware/user2.bin')], check=True)

    for target in ('512KB','1MB','2MB'):
        (work/'generated_flash_layout.h').write_text(flash_layout.header(flash_layout.layout(target)))
        subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                        '-fsanitize=address,undefined','-DSERIAL2WIFI_OTA_ENABLED=1','-DSERIAL2WIFI_OTA_TESTING=0',
                        '-I'+str(work),'-I'+str(ROOT/'tests/api-stubs'),'-I'+str(ROOT/'src/serial'),str(work/'test.c'),'-o',str(work/'test')],check=True)
        subprocess.run([str(work/'test'),str(ROOT/'firmware/user1.bin'),str(ROOT/'firmware/user2.bin')],check=True)
