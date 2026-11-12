#!/usr/bin/env python3
"""Package one firmware capacity, or combine verified capacities for publication."""
import argparse,gzip,hashlib,io,json,os,re,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def write_archive(name,files):
 if any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in name):raise ValueError('Invalid archive name')
 stamp=int(os.environ.get('SOURCE_DATE_EPOCH','0'));dist=ROOT/'dist';dist.mkdir(exist_ok=True)
 output=dist/(name+'.tar.gz');temporary=output.with_suffix('.tmp')
 with temporary.open('wb') as raw,gzip.GzipFile(filename='',fileobj=raw,mode='wb',mtime=stamp) as compressed,tarfile.open(fileobj=compressed,mode='w') as tar:
  for relative,(data,mode) in sorted(files.items()):
   info=tarfile.TarInfo(name+'/'+relative);info.size=len(data);info.mtime=stamp;info.mode=mode
   tar.addfile(info,io.BytesIO(data))
 temporary.replace(output)
 digest=hashlib.sha256(output.read_bytes()).hexdigest();output.with_suffix(output.suffix+'.sha256').write_text(digest+'  '+output.name+'\n')
 print('Release:',output.relative_to(ROOT));print('SHA256:',digest)

def package(sdk):
 m=json.loads((ROOT/'firmware/release-manifest.json').read_text())
 capacity=m['geometry']['target'].lower();name='esp-serial2wifi-'+m['version']+'-'+capacity+('-banked-ui' if m['ui']['storage']=='slot-banked' else '')
 paths={n:ROOT/'firmware'/n for n in m['images']}
 for n,info in m['images'].items():
  if hashlib.sha256(paths[n].read_bytes()).hexdigest()!=info['sha256']:raise ValueError('Image differs from manifest: '+n)
 paths.update({n:ROOT/n for n in ('LICENSE','sdk-lock.json','requirements-flash.txt')})
 paths['README.md']=ROOT/'tools/templates/release_readme.md';paths['release-manifest.json']=ROOT/'firmware/release-manifest.json'
 for n in ('flash_ota.py','flash_mcu.sh','flash_serial.py'):paths[n]=ROOT/'tools'/n
 for n in ('ui_package.py','flash_layout.py'):paths['lib/'+n]=ROOT/'tools/lib'/n
 for n in ('boot_v1.6.bin','blank.bin','esp_init_data_default_v05.bin'):
  paths[n]=sdk/'bin'/n
  if hashlib.sha256(paths[n].read_bytes()).hexdigest()!=m['system_images'][n]['sha256']:raise ValueError('SDK initialization file differs from manifest: '+n)
 paths['licenses/espressif-sdk.txt']=sdk/'License'
 version='6.0.0' if m['ui']['storage']=='slot-banked' else '5.5.0'
 paths['licenses/xterm.txt']=ROOT/'third_party'/('xterm-'+version)/'LICENSE'
 files={n:(p.read_bytes(),0o755 if n in ('flash_ota.py','flash_mcu.sh','flash_serial.py') else 0o644) for n,p in paths.items()}
 if m['ui']['storage']=='slot-banked':
  data,mode=files['README.md'];files['README.md']=(b'Experimental banked-UI build. OTA requires update-user1.elu/update-user2.elu. The USB instructions below apply only to embedded UI; use the source package installer for this layout.\n\n'+data.replace(b'user1.bin user2.bin',b'update-user1.elu update-user2.elu'),mode)
 write_archive(name,files)

def combine(archives,label):
 files={};manifests={}
 for archive in archives:
  expected=archive.with_suffix(archive.suffix+'.sha256').read_text().split()
  if expected!=[hashlib.sha256(archive.read_bytes()).hexdigest(),archive.name]:raise ValueError('Archive checksum mismatch')
  prefix=archive.name.removesuffix('.tar.gz')+'/'
  with tarfile.open(archive) as tar:
   members={}
   for entry in tar:
    if not entry.isfile() or not entry.name.startswith(prefix):raise ValueError('Unexpected archive entry')
    relative=entry.name[len(prefix):]
    if not relative or any(p in ('','.','..') for p in relative.split('/')) or relative in members:raise ValueError('Unsafe or duplicate archive entry')
    members[relative]=(tar.extractfile(entry).read(),entry.mode&0o777)
  m=json.loads(members['release-manifest.json'][0]);capacity=m['geometry']['target'].lower()
  if capacity not in ('512kb','4mb') or capacity in manifests or m['ui']['storage']!='embedded':raise ValueError('Expected one standard image set per capacity')
  for n,info in (m['images']|m['system_images']).items():
   if hashlib.sha256(members[n][0]).hexdigest()!=info['sha256']:raise ValueError('Packaged image differs: '+n)
  manifests[capacity]=m
  files.update({capacity+'/'+n:value for n,value in members.items()})
 if set(manifests)!= {'512kb','4mb'}:raise ValueError('Both 512 KB and 4 MB images are required')
 a,b=manifests.values()
 for key in ('version','source_revision','source_tree_sha256','inputs'):
  if a[key]!=b[key]:raise ValueError('Capacity builds differ: '+key)
 if label!=a['version'] and not (re.fullmatch(r'[0-9a-f]{8}',label) and a['source_revision'] and a['source_revision'].startswith(label)):raise ValueError('Label must match the firmware version or short source revision')
 readme=f'''# ESP Serial2WiFi {a['version']}

This archive contains firmware for **both 512 KB and 4 MB ESP8266 boards**.
Use the folder matching your board's actual flash capacity:

- `512kb/`: serial flashing; no OTA slot.
- `4mb/`: serial flashing and OTA updates.

Each folder is self-contained. Open its README for backup, installation and
update instructions. Run its flashing tools from that folder, for example:

```sh
cd 4mb
python3 flash_ota.py <device-address> user1.bin user2.bin
```

That example updates a 4 MB device already running a compatible layout.
For factory installation, follow the selected folder's README; it erases settings.
Never mix images from the two folders. ESP restarts send UART boot bytes: put the
attached device in a safe state before updating.

Source commit: `{a['source_revision']}`

Project and dependency licenses are included in each capacity folder.
'''
 files['README.md']=(readme.encode(),0o644)
 write_archive('esp-serial2wifi-'+label,files)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path);p.add_argument('--combine',type=Path,nargs=2,metavar=('512KB_ARCHIVE','4MB_ARCHIVE'));p.add_argument('--label');a=p.parse_args()
 if a.combine:
  if not a.label or a.sdk:p.error('--combine requires --label and cannot use --sdk')
  combine(a.combine,a.label)
 else:
  if not a.sdk or a.label:p.error('Single-capacity packaging requires --sdk')
  package(a.sdk)
if __name__=='__main__':main()
