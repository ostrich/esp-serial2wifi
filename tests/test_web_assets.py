#!/usr/bin/env python3
"""Production packaging must exclude obsolete admin assets, including stale files."""
import _paths
from pathlib import Path
import subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1]
base=ROOT/'.build-tests/web-assets';base.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=base) as tmp:
    output=Path(tmp)/'public';output.mkdir();(output/'console.js').write_text('stale admin code')
    subprocess.run([sys.executable,str(ROOT/'tools/build/build_ui.py'),'web','--output',str(output)],check=True)
    import re
    version=re.search(r'src="([a-f0-9]{24})/app.js"',(output/'index.html').read_text()).group(1)
    expected={'favicon.ico','pure.css','style.css','ui.js','userpage.js','index.html'}|{version+'/'+name for name in ('style.css','xterm.css','app.js','xterm.js')}
    actual={p.relative_to(output).as_posix() for p in output.rglob('*') if p.is_file()}
    assert actual==expected,(actual-expected,expected-actual)
    assert b'Permission is hereby granted' in (output/version/'xterm.js').read_bytes()
    assert 'Preview' not in (output/'index.html').read_text()
    assert 'createPresets' not in (output/'ui.js').read_text() and 'makeAjaxInput' not in (output/'ui.js').read_text()
    assert 'function notifyButtonPressed' in (output/'userpage.js').read_text()
print('PASS production asset allowlist: replacement UI, compatibility helpers, no stale/admin assets')

# Only operational non-versioned interfaces remain; old admin handlers are absent.
main=(ROOT/'src/core/main.c').read_text()
for path in ('/next','/next/','/console/send','/console/baud','/wifi/connect',
             '/api/v1/settings/identity','/api/v1/settings/discovery','/system/update','/services/update','/web-server/upload','/pins','/mqtt'):
    assert ('{ "'+path+'",') not in main,path
for path in ('/','/menu','/api/v1/settings/description','/api/v1/network/identity','/api/v1/firmware/upload','/pgm/upload','/pgmmega/upload','/user/*'):
    assert ('{ "'+path+'",') in main,path
print('PASS route table: obsolete admin handlers retired; OTA, programmers and custom pages retained')
