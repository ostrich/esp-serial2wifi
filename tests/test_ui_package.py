#!/usr/bin/env python3
"""Real-artifact package/client checks and non-destructive migration preflight."""
import _paths
from pathlib import Path
import hashlib,json,struct,subprocess
from ui_package import parse
from install_ui import preflight,pages_used
from flash_ota import validate,RUNTIME_VERSION
ROOT=Path(__file__).resolve().parents[1]
backup=bytearray(b'\xff'*0x400000)
backup[0x100000:0x100010]=struct.pack('<IBBHII',0x73665345,1,0,0,0,0)
for slot in range(2):
 path=ROOT/f'firmware/update-user{slot+1}.elu';data=path.read_bytes();parsed,app,ui=preflight(backup,data)
 assert parsed==slot and ui==(ROOT/'build/ui.espfs').read_bytes()
 assert len(app)==struct.unpack_from('<I',data,8)[0] and hashlib.sha256(ui).digest()==data[20:52]
 for offset in (8,12,16,20,52,1024+30,len(data)-1):
  broken=bytearray(data);broken[offset]^=1
  try:parse(broken)
  except ValueError:pass
  else:raise AssertionError(f'Corruption accepted at {offset}')
 code="""import {readFileSync} from 'node:fs';import {checkFirmware} from './web/app/update-model.js';const data=new Uint8Array(readFileSync(process.argv[1]));const info={external_ui:true,target_slot:Number(process.argv[2]),max_image_bytes:768000,image_abi:'ELSDK306-MAP4-CFG2'};checkFirmware(data,info);data[16]^=1;try{checkFirmware(data,info);process.exit(1);}catch{};"""
 subprocess.run(['node','--input-type=module','-e',code,str(path),str(slot+1)],cwd=ROOT,check=True)
validate({'sdk':RUNTIME_VERSION,'active_slot':1,'target_slot':2,'ota_supported':True,'image_abi':'ELSDK306-MAP4-CFG2'},[ROOT/'firmware/update-user1.elu',ROOT/'firmware/update-user2.elu'])
# Existing pages are never reinterpreted as unused space when malformed/overlapping.
assert pages_used(backup)==16
for change in ('overlap','corrupt','truncated'):
 broken=bytearray(backup)
 if change=='overlap':
  n=0x27c000;broken[0x100000:0x100014]=struct.pack('<IBBHII',0x73665345,0,0,4,n,n)+b'a\0\0\0';at=0x100014+n;broken[at:at+16]=struct.pack('<IBBHII',0x73665345,1,0,0,0,0)
 elif change=='corrupt':broken[0x100000]=0
 else:broken=broken[:-1]
 try:preflight(broken,data)
 except ValueError:pass
 else:raise AssertionError(change+' migration accepted')
print('PASS real packages, JavaScript file validation, corruption rejection, existing-page preservation/overlap refusal and complete-backup requirement')
