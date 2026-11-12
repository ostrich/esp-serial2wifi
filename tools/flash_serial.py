#!/usr/bin/env python3
"""USB application update or explicit factory installation; requires a named port."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
SCRIPT_DIR=Path(__file__).resolve().parent
ROOT=SCRIPT_DIR if (SCRIPT_DIR/'release-manifest.json').exists() else SCRIPT_DIR.parent
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--slot',type=int,choices=(1,2),default=1);p.add_argument('--baud',type=int,default=230400);p.add_argument('--sdk',type=Path);p.add_argument('--factory',action='store_true');a=p.parse_args()
 if not a.port:p.error('Set PORT to the intended serial device; no device is selected automatically')
 images=ROOT/'firmware' if (ROOT/'firmware/release-manifest.json').exists() else ROOT
 m=json.loads((images/'release-manifest.json').read_text());name=f'user{a.slot}.bin'
 if m['ui']['storage']!='embedded':p.error('Separate-UI images require the advanced package installer')
 if name not in m['images']:p.error('The selected slot does not exist for this build')
 image=images/name;entry=m['images'][name]
 if hashlib.sha256(image.read_bytes()).hexdigest()!=entry['sha256']:p.error('Image hash differs from manifest')
 sdk=a.sdk or ROOT/'.cache/esp8266'/m['inputs']['sdk']['directory']
 sdk_bin=sdk/'bin' if images!=ROOT or a.sdk else ROOT
 command=[sys.executable,'-m','esptool','--chip','esp8266','--port',a.port,'--baud',str(a.baud)]
 writes=[hex(entry['address']),str(image)]
 if a.factory:
  if a.slot!=1:p.error('Factory installation starts in slot 1')
  for filename,info in m['system_images'].items():
   if hashlib.sha256((sdk_bin/filename).read_bytes()).hexdigest()!=info['sha256']:p.error('Pinned SDK file differs: '+filename)
  print('Erasing all flash, including saved configuration and custom pages.',flush=True)
  subprocess.run(command+['erase-flash'],check=True)
  writes=['0x0',str(sdk_bin/'boot_v1.6.bin'),*writes,hex(m['geometry']['regions']['phy'][0]),str(sdk_bin/'esp_init_data_default_v05.bin'),hex(m['geometry']['regions']['params'][0]+4096),str(sdk_bin/'blank.bin')]
 mode={0:'qio',1:'qout',2:'dio',3:'dout'}[m['spi_mode']];frequency={0:'40m',1:'26m',2:'20m',15:'80m'}[m['spi_frequency_code']]
 flags=['--flash-size',m['geometry']['target'],'--flash-mode',mode,'--flash-freq',frequency] if a.factory else []
 subprocess.run(command+['write-flash',*flags,*writes],check=True)
 print('Write completed. Verify the running firmware over Wi-Fi; app-only writes do not select the boot slot.')
if __name__=='__main__':main()
