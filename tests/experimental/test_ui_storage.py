#!/usr/bin/env python3
"""Exercise actual banked OTA/storage code with real packages and flash faults."""
import _paths
from pathlib import Path
import subprocess,tempfile,hashlib
from flash_stubs import STUB, HTTP, SDK
ROOT=Path(__file__).resolve().parents[2]
TEST=r'''
void (*programmingCB)(char *,short);
int serbridgeInMCUFlashing(void){return 0;}
bool serialInputPending(void){return false;}
uint8_t serbridge_client_count(void){return 0;}
bool apiWrite(HttpdConnData *c){return c->conn&&c->requestType==HTTPD_METHOD_POST;}

void errorResponse(HttpdConnData *c,int code,char *m){(void)m;c->code=code;}
static void reset(void){assert(!active_upload&&!allocations);verified_slot=-1;activating=false;operationOwner=NULL;upgrade_flag=scheduled=0;current_slot=0;writes=erases=fail=fail_write=fail_read=corrupt_read=allocation_failure=0;test_map=4;capacity=22;memset(memory,0x5a,sizeof memory);}
static void protected(unsigned slot){unsigned app=slot?FLASH_APP2_START:FLASH_APP1_START,ui=slot?FLASH_UI2_START:FLASH_UI1_START;for(unsigned i=0;i<sizeof memory;i++)if(!((i>=app&&i<app+FLASH_APP1_SIZE)||(i>=ui&&i<ui+FLASH_UI1_SIZE)))assert(memory[i]==0x5a);}
static void begin(HttpdConnData *c,HttpdPostData *p,uint8_t *data,unsigned n){memset(c,0,sizeof *c);c->conn=c;c->requestType=HTTPD_METHOD_POST;c->post=p;p->len=n;p->buffLen=1024;p->received=1024;p->buff=(char *)data;}
static void abort_upload(HttpdConnData *c){c->conn=NULL;cgiUploadFirmware(c);assert(!active_upload&&!allocations);}
static unsigned transfer(uint8_t *data,unsigned size,unsigned slot,unsigned stop,int expected){
 reset();current_slot=1-slot;HttpdConnData c;HttpdPostData p;begin(&c,&p,data,size);
 for(unsigned offset=0;offset<size&&offset<stop;offset+=1024){p.buff=(char *)data+offset;p.received=offset+1024;int r=cgiUploadFirmware(&c);if(r==HTTPD_CGI_DONE)break;}
 if(expected)assert(c.code==expected);else assert(!c.code&&active_upload);
 protected(slot);
 if(expected==200){assert(verified_slot==(int)slot);assert(!memcmp(memory+(slot?FLASH_APP2_START:FLASH_APP1_START),data+1024,le32(data+8)));cgiRebootFirmware(&c);assert(c.code==200&&scheduled==1&&upgrade_flag==2);current_slot=slot;assert(uiStorageMount());EspFsFile *f=espFsOpen(firmwareFsCtx,"index.html");assert(f);char chunk[511];assert(espFsRead(f,chunk,sizeof chunk)>0&&(uint8_t)chunk[0]==31&&(uint8_t)chunk[1]==139);while(espFsRead(f,chunk,sizeof chunk)>0){}espFsClose(f);}
 else {assert(verified_slot==-1);cgiRebootFirmware(&c);assert(c.code==409&&!scheduled);}
 abort_upload(&c);protected(slot);return slot?FLASH_UI2_START:FLASH_UI1_START;
}
static uint8_t *load(const char *path,unsigned *size){FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);uint8_t *data=malloc(*size);assert(data&&fread(data,1,*size,f)==*size);fclose(f);return data;}
int main(int argc,char **argv){assert(argc==3);unsigned n[2];uint8_t *data[2]={load(argv[1],n),load(argv[2],n+1)};
 // SHA-256 known answer, with byte-at-a-time feeding and padding boundary cases.
 AssetDigest h;uint8_t sha[32];assetDigestInit(&h);assetDigestFeed(&h,"a",1);assetDigestFeed(&h,"bc",2);assetDigestFinish(&h,sha);static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};assert(!memcmp(sha,abc,32));
 for(unsigned slot=0;slot<2;slot++){
  unsigned ui=transfer(data[slot],n[slot],slot,n[slot],200);
  // Corrupt banks fail closed; a correctly rehashed malformed extent also fails.
  uint32_t len=le32(data[slot]+12);memory[ui+4096]^=1;assert(!uiStorageMount());assetDigestInit(&h);assetDigestFeed(&h,memory+ui+4096,len);assetDigestFinish(&h,sha);assert(!uiStorageValidate(slot,len,sha));
  unsigned boundary=1024+((le32(data[slot]+8)+1023)&~1023U);unsigned stops[]={1024,2048,boundary,boundary+1024,n[slot]-1024};
  for(unsigned i=0;i<sizeof stops/sizeof *stops;i++)transfer(data[slot],n[slot],slot,stops[i],0);
  // UI digest, application CRC, package padding, wrong slot and malformed header.
  unsigned bad[]={20,1024+60,boundary+60,52,16};
  for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){data[slot][bad[i]]^=1;transfer(data[slot],n[slot],slot,n[slot],400);data[slot][bad[i]]^=1;}
 }
 // Failed writes and OOM must never activate an incomplete pair.
 reset();HttpdConnData c;HttpdPostData p;begin(&c,&p,data[1],n[1]);allocation_failure=1;cgiUploadFirmware(&c);assert(c.code==503&&!writes&&!erases);abort_upload(&c);
 reset();begin(&c,&p,data[1],n[1]);cgiUploadFirmware(&c);fail_write=1;p.buff=(char *)data[1]+1024;p.received=2048;cgiUploadFirmware(&c);assert(c.code==500&&verified_slot<0);abort_upload(&c);protected(1);
 for(unsigned mode=0;mode<3;mode++){
  reset();begin(&c,&p,data[1],n[1]);cgiUploadFirmware(&c);if(mode==0)fail=1;else if(mode==1)fail_read=1;else corrupt_read=1;
  p.buff=(char *)data[1]+1024;p.received=2048;cgiUploadFirmware(&c);assert(c.code==500&&verified_slot<0);abort_upload(&c);protected(1);
 }
 // Hard reset loses in-RAM upload state without running abort cleanup.
 reset();begin(&c,&p,data[1],n[1]);unsigned cut=1024+((le32(data[1]+8)+1023)&~1023U)+1024;
 for(unsigned at=0;at<cut;at+=1024){p.buff=(char *)data[1]+at;p.received=at+1024;assert(cgiUploadFirmware(&c)==HTTPD_CGI_MORE);}
 assert(active_upload);test_free(active_upload);active_upload=NULL;c.cgiPrivData=NULL;verified_slot=-1;operationOwner=NULL;cgiRebootFirmware(&c);assert(c.code==409&&!scheduled);protected(1);current_slot=1;assert(!uiStorageMount());
 reset();begin(&c,&p,data[1],n[1]);cgiUploadFirmware(&c);current_slot=1;p.buff=(char *)data[1]+1024;p.received=2048;cgiUploadFirmware(&c);assert(c.code==409&&!writes&&!erases);abort_upload(&c);
 free(data[0]);free(data[1]);puts("PASS SHA-256, matched banks/both slots, interruption, protected data, malformed/corrupt packages, write faults, OOM and activation guards");}
'''
out=ROOT/'.build-tests/ui-storage';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as temporary:
 work=Path(temporary);(work/'esp8266.h').write_text(STUB.replace('if(fail_read)return 1;', 'assert(!(a&3)&&!(n&3)&&!((uintptr_t)p&3));if(fail_read)return 1;')+SDK+'\n#define os_memset memset\n#define os_malloc test_alloc\n#define os_strcmp strcmp\n#define os_strncmp strncmp\n#define os_strcpy strcpy\n#define os_printf printf\n')
 (work/'httpd.h').write_text(HTTP);(work/'cgi.h').write_text('#include "httpd.h"\nvoid errorResponse(HttpdConnData *,int,char *);\n');(work/'cgiflash.h').write_text('')
 (work/'generated_flash_layout.h').write_text((ROOT/'build/generated_flash_layout.h').read_text())
 assert '#define FLASH_EXTERNAL_UI 1' in (work/'generated_flash_layout.h').read_text(),'Build SERIAL2WIFI_EXTERNAL_UI=1 first'
 (work/'ui_identity.h').write_bytes((ROOT/'build/ui_identity.h').read_bytes())
 sources=(ROOT/'src/espfs/espfs.c').read_text()+'\n'+'\n'.join((ROOT/'src/core'/name).read_text() for name in ('operation.c','flash_layout.c','asset_digest.c','ui_storage.c','firmware_image.c','cgiflash.c'))
 (work/'test.c').write_text('#define SERIAL2WIFI_OTA_ENABLED 1\n#include <esp8266.h>\n'+sources+'\n'+TEST)
 binary=work/'test';subprocess.run(['cc','-D__ets__','-std=c99','-g','-O1','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-int-to-pointer-cast','-Wno-pointer-to-int-cast','-Wno-misleading-indentation','-I'+str(work),'-I'+str(ROOT/'src/core'),'-I'+str(ROOT/'src/espfs'),'-I'+str(ROOT/'src/serial'),'-I'+str(ROOT/'tests/support/api-stubs'),str(work/'test.c'),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(ROOT/'firmware/update-user1.elu'),str(ROOT/'firmware/update-user2.elu')],check=True)
