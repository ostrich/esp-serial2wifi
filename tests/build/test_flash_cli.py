#!/usr/bin/env python3
"""Verify USB flashing decisions and protected defaults without invoking esptool."""
import contextlib,hashlib,importlib.util,io,json,sys,tempfile
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('flash_cli',ROOT/'tools/flash_serial.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
out=ROOT/'.build-tests/flash-cli';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as folder:
 root=Path(folder);module.ROOT=root
 sdk=root/'.cache/esp8266/sdk/bin';sdk.mkdir(parents=True)
 images=root/'firmware';images.mkdir();image=images/'user1.bin';image.write_bytes(b'synthetic app')
 digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
 manifest={'ui':{'storage':'embedded'},'inputs':{'sdk':{'directory':'sdk'}},'images':{'user1.bin':{'sha256':digest(image),'address':4096}},'system_images':{},'geometry':{'target':'4MB','regions':{'phy':[0x3fc000,4096],'params':[0x3fd000,12288]}},'spi_mode':2,'spi_frequency_code':0}
 for name in ('boot_v1.6.bin','blank.bin','esp_init_data_default_v05.bin'):
  p=sdk/name;p.write_bytes(name.encode());manifest['system_images'][name]={'sha256':digest(p)}
 metadata=images/'release-manifest.json';metadata.write_text(json.dumps(manifest))
 def run(args):
  calls=[];failed=False
  with patch('sys.argv',['flash_serial.py',*args]),patch('subprocess.run',side_effect=lambda command,**kw:calls.append(command)),contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
   try:module.main()
   except SystemExit as error:failed=error.code!=0
  return failed,calls
 assert run(['--port',''])[0] and not run(['--port',''])[1]
 failed,calls=run(['--port','fixture-port']);assert not failed and len(calls)==1 and 'erase-flash' not in calls[0]
 failed,calls=run(['--port','fixture-port','--factory']);assert not failed and len(calls)==2 and calls[0][-1]=='erase-flash'
 assert calls[1][calls[1].index('--flash-freq')+1]=='40m'
 assert '0x3fc000' in calls[1] and '0x3fe000' in calls[1] and '0x1000' in calls[1] and '0x0' in calls[1]
 assert run(['--port','fixture-port','--slot','2'])[0] and not run(['--port','fixture-port','--slot','2'])[1]
 image.write_bytes(b'changed app');failed,calls=run(['--port','fixture-port','--factory']);assert failed and not calls
 image.write_bytes(b'synthetic app');(sdk/'blank.bin').write_bytes(b'changed SDK');failed,calls=run(['--port','fixture-port','--factory']);assert failed and not calls
 # The extracted release layout must use its own included files, not a source cache.
 (sdk/'blank.bin').write_bytes(b'blank.bin')
 for p in sdk.iterdir():(root/p.name).write_bytes(p.read_bytes())
 (root/'user1.bin').write_bytes(image.read_bytes());(root/'release-manifest.json').write_bytes(metadata.read_bytes());metadata.unlink()
 failed,calls=run(['--port','fixture-port','--factory']);assert not failed and len(calls)==2 and str(root/'boot_v1.6.bin') in calls[1]
 print('PASS USB CLI: explicit port, app-only preservation, factory addresses, pre-erase hash refusal and standalone release layout')
