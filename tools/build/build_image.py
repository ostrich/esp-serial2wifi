#!/usr/bin/env python3
"""Extract SDK image sections in an isolated build directory and check the result."""
import argparse,os,shutil,subprocess,sys,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('elf',type=Path);p.add_argument('output',type=Path)
p.add_argument('--toolchain',type=Path,required=True)
for name in ('slot','mode','frequency','map','limit'):p.add_argument('--'+name,type=int,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(dir=root/'build',prefix='image-') as folder:
 folder=Path(folder)
 for section,suffix in [('.text','text'),('.data','data'),('.rodata','rodata'),('.irom0.text','irom0text')]:
  subprocess.run([str(a.toolchain/'xtensa-lx106-elf-objcopy'),'--only-section',section,'-O','binary',str(a.elf.resolve()),str(folder/('eagle.app.v6.'+suffix+'.bin'))],check=True)
 env={**os.environ,'COMPILE':'gcc','PATH':str(a.toolchain.resolve())+os.pathsep+os.environ['PATH']}
 subprocess.run([sys.executable,str(root/'tools/build/generate_appbin.py'),str(a.elf.resolve()),'2',str(a.mode),str(a.frequency),str(a.map),str(a.slot)],cwd=folder,env=env,check=True,stdout=subprocess.DEVNULL)
 image=folder/'eagle.app.flash.bin';size=image.stat().st_size
 if size>a.limit:raise SystemExit(f'Image exceeds slot: {size}/{a.limit} bytes')
 a.output.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(image,a.output)
 print(f'{a.output}: {size}/{a.limit} bytes ({a.limit-size} free)')
