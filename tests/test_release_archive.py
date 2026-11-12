#!/usr/bin/env python3
"""Check a local ESP Serial2WiFi binary release archive against its manifest."""
import _paths
import argparse,hashlib,json,tarfile,tempfile,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);a=p.parse_args()
assert a.archive.name.startswith('esp-serial2wifi-') and a.archive.name.endswith('.tgz')
prefix=a.archive.name[:-4]+'/'
with tarfile.open(a.archive) as tar:
 names=tar.getnames();assert all(n==prefix[:-1] or n.startswith(prefix) for n in names)
 manifest=json.load(tar.extractfile(prefix+'release-manifest.json'))
 for name,info in manifest['images'].items():assert hashlib.sha256(tar.extractfile(prefix+name).read()).hexdigest()==info['sha256'],name
 for name,info in manifest['system_images'].items():assert hashlib.sha256(tar.extractfile(prefix+name).read()).hexdigest()==info['sha256'],name
 for name in ('README.md','LICENSE','sdk-lock.json','flash_ota.py','flash_mcu.sh','flash_serial.py','requirements-flash.txt','lib/ui_package.py','lib/flash_layout.py'):assert prefix+name in names,name
 assert b'ESP Serial2WiFi' in tar.extractfile(prefix+'README.md').read()
 assert tar.extractfile(prefix+'LICENSE').read()==(Path(__file__).resolve().parents[1]/'LICENSE').read_bytes()
 if manifest['geometry']['bytes']==524288:assert prefix+'user2.bin' not in names
with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1]/'.build-tests') as directory:
 with tarfile.open(a.archive) as tar:tar.extractall(directory,filter='data')
 script=Path(directory)/prefix/'flash_ota.py'
 result=subprocess.run([sys.executable,str(script),'--help'],capture_output=True,text=True,check=True)
 assert 'versioned ESP Serial2WiFi API' in result.stdout
 result=subprocess.run([sys.executable,str(Path(directory)/prefix/'flash_serial.py'),'--help'],capture_output=True,text=True,check=True)
 assert 'factory' in result.stdout
checksum=a.archive.with_suffix(a.archive.suffix+'.sha256').read_text().split()
assert checksum==[hashlib.sha256(a.archive.read_bytes()).hexdigest(),a.archive.name]
print('PASS capacity-specific archive directory, slot selection, manifest/image hashes, licenses, instructions and update tools')
