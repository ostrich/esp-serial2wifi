#!/usr/bin/env python3
"""Verify capacity-specific or combined firmware downloads, without flashing."""
import _paths
import argparse,hashlib,json,tarfile,tempfile,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('archive',type=Path);a=p.parse_args()
assert a.archive.name.startswith('esp-serial2wifi-') and a.archive.name.endswith('.tar.gz')
prefix=a.archive.name.removesuffix('.tar.gz')+'/'
with tarfile.open(a.archive) as tar:
 names=tar.getnames();assert len(names)==len(set(names))
 assert all(n.startswith(prefix) and '..' not in Path(n).parts for n in names)
 folders=[''] if prefix+'release-manifest.json' in names else ['512kb/','4mb/']
 manifests=[]
 for folder in folders:
  base=prefix+folder;manifest=json.load(tar.extractfile(base+'release-manifest.json'));manifests.append(manifest)
  if folder:assert manifest['geometry']['target'].lower()+'/'==folder
  for name,info in (manifest['images']|manifest['system_images']).items():assert hashlib.sha256(tar.extractfile(base+name).read()).hexdigest()==info['sha256'],name
  for name in ('README.md','LICENSE','sdk-lock.json','flash_ota.py','flash_mcu.sh','flash_serial.py','requirements-flash.txt','lib/ui_package.py','lib/flash_layout.py'):assert base+name in names,name
  assert b'ESP Serial2WiFi' in tar.extractfile(base+'README.md').read()
  assert tar.extractfile(base+'LICENSE').read()==(ROOT/'LICENSE').read_bytes()
  if manifest['geometry']['bytes']==524288:assert base+'user2.bin' not in names
  else:assert base+'user2.bin' in names
 if len(manifests)==2:
  for key in ('version','source_revision','source_tree_sha256','inputs'):assert manifests[0][key]==manifests[1][key],key
  assert b'512kb/' in tar.extractfile(prefix+'README.md').read()
(ROOT/'.build-tests').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=ROOT/'.build-tests') as directory:
 with tarfile.open(a.archive) as tar:tar.extractall(directory,filter='data')
 for folder in folders:
  base=Path(directory)/prefix/folder
  result=subprocess.run([sys.executable,str(base/'flash_ota.py'),'--help'],cwd=base,capture_output=True,text=True,check=True)
  assert 'versioned ESP Serial2WiFi API' in result.stdout
  result=subprocess.run([sys.executable,str(base/'flash_serial.py'),'--help'],cwd=base,capture_output=True,text=True,check=True)
  assert 'factory' in result.stdout
checksum=a.archive.with_suffix(a.archive.suffix+'.sha256').read_text().split()
assert checksum==[hashlib.sha256(a.archive.read_bytes()).hexdigest(),a.archive.name]
print('PASS archive layout, capacities, manifest/image hashes, licenses, instructions and standalone update tools')
