#!/usr/bin/env python3
"""Create a deterministic, capacity-specific firmware archive for local or CI builds."""
import argparse,gzip,hashlib,io,json,os,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sdk',type=Path,required=True);a=p.parse_args()
 m=json.loads((ROOT/'firmware/release-manifest.json').read_text())
 capacity=m['geometry']['target'].lower();name='esp-serial2wifi-'+m['version']+'-'+capacity+('-banked-ui' if m['ui']['storage']=='slot-banked' else '')
 if any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-' for c in name):p.error('Invalid release version')
 files={n:ROOT/'firmware'/n for n in m['images']}
 for n,info in m['images'].items():
  if hashlib.sha256(files[n].read_bytes()).hexdigest()!=info['sha256']:raise ValueError('Image differs from manifest: '+n)
 files.update({n:ROOT/n for n in ('LICENSE','sdk-lock.json','requirements-flash.txt')})
 files['README.md']=ROOT/'tools/templates/release_readme.md';files['release-manifest.json']=ROOT/'firmware/release-manifest.json'
 for n in ('flash_ota.py','flash_mcu.sh','flash_serial.py'):files[n]=ROOT/'tools'/n
 for n in ('ui_package.py','flash_layout.py'):files['lib/'+n]=ROOT/'tools/lib'/n
 for n in ('boot_v1.6.bin','blank.bin','esp_init_data_default_v05.bin'):
  files[n]=a.sdk/'bin'/n
  if hashlib.sha256(files[n].read_bytes()).hexdigest()!=m['system_images'][n]['sha256']:raise ValueError('SDK initialization file differs from manifest: '+n)
 files['licenses/espressif-sdk.txt']=a.sdk/'License'
 for version in ('5.5.0','6.0.0'):
  if version==('6.0.0' if m['ui']['storage']=='slot-banked' else '5.5.0'):files['licenses/xterm.txt']=ROOT/'third_party'/('xterm-'+version)/'LICENSE'
 stamp=int(os.environ.get('SOURCE_DATE_EPOCH','0'));dist=ROOT/'dist';dist.mkdir(exist_ok=True)
 output=dist/(name+'.tgz');temporary=output.with_suffix('.tmp')
 with temporary.open('wb') as raw,gzip.GzipFile(filename='',fileobj=raw,mode='wb',mtime=stamp) as compressed,tarfile.open(fileobj=compressed,mode='w') as tar:
  for relative,path in sorted(files.items()):
   data=path.read_bytes()
   if relative=='README.md' and m['ui']['storage']=='slot-banked':
    data=b'Experimental banked-UI build. OTA requires update-user1.elu/update-user2.elu. The USB instructions below apply only to embedded UI; use the source package installer for this layout.\n\n'+data.replace(b'user1.bin user2.bin',b'update-user1.elu update-user2.elu')
   info=tarfile.TarInfo(name+'/'+relative);info.size=len(data);info.mtime=stamp;info.mode=0o755 if relative in ('flash_ota.py','flash_mcu.sh','flash_serial.py') else 0o644
   tar.addfile(info,io.BytesIO(data))
 temporary.replace(output)
 digest=hashlib.sha256(output.read_bytes()).hexdigest();output.with_suffix(output.suffix+'.sha256').write_text(digest+'  '+output.name+'\n')
 print('Release:',output.relative_to(ROOT));print('SHA256:',digest)
if __name__=='__main__':main()
