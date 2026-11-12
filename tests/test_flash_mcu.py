#!/usr/bin/env python3
"""Exercise protocol selection and failures with a fake curl; never contact a device."""
import os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(dir=ROOT/'.build-tests') as temporary:
 folder=Path(temporary);log=folder/'requests';image=folder/'app.hex';image.write_text(':00000001FF\n')
 curl=folder/'curl';curl.write_text('''#!/usr/bin/env python3
import os,sys
from pathlib import Path
with open(os.environ['MCU_TEST_LOG'],'a') as f:f.write(' '.join(sys.argv[1:])+'\\n')
if sys.argv[-1].endswith('/upload'):
 print('Success')
 sys.exit(int(os.environ.get('MCU_TEST_FAILURE','0')))
print('204' if '-XPOST' in sys.argv else 'SYNC')
''');curl.chmod(0o755)
 env={**os.environ,'PATH':str(folder)+os.pathsep+os.environ['PATH'],'MCU_TEST_LOG':str(log)}
 for protocol,endpoint in [('optiboot','pgm'),('stk500v2','pgmmega')]:
  for failure in ('0','7'):
   log.write_text('')
   result=subprocess.run(['bash',str(ROOT/'tools/flash_mcu.sh'),'-p',protocol,'fixture.invalid',str(image)],env={**env,'MCU_TEST_FAILURE':failure},capture_output=True,text=True,timeout=5)
   assert (result.returncode==0)==(failure=='0'),result.stderr
   lines=log.read_text().splitlines();assert len(lines)==3,lines
   assert all('http://fixture.invalid/'+endpoint+'/' in line for line in lines),lines
   assert lines[0].count('-XPOST')==1 and '/upload' in lines[2]
 log.write_text('')
 result=subprocess.run(['bash',str(ROOT/'tools/flash_mcu.sh'),'-p','unknown','fixture.invalid',str(image)],env=env,capture_output=True,timeout=5)
 assert result.returncode and not log.read_text()
print('PASS shared MCU CLI: both protocols, single reset/upload, curl failure and invalid protocol')
