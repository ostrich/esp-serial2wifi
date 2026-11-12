#!/usr/bin/env python3
"""Packaged, loopback-only regressions for the UI lifecycle/size review."""
import _paths
import argparse,json,hashlib,subprocess,tempfile,threading
from pathlib import Path
from fixture_server import FixtureServer,ROOT
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--browser',required=True);args=p.parse_args()
out=ROOT/'.build-tests/ui-review';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as tmp:
 assets=Path(tmp);subprocess.run(['python3',str(ROOT/'tools/build/build_ui.py'),'web','--output',str(assets)],check=True)
 version=next(p for p in assets.iterdir() if p.is_dir());h=hashlib.sha256()
 for file in sorted(version.iterdir()):h.update(file.name.encode()+b'\0');h.update(file.read_bytes())
 assert version.name==h.hexdigest()[:24], 'Asset identity was computed before final optimization'
 (assets/'user').mkdir();(assets/'user/probe.html').write_text('''<!doctype html><html><head><meta name="viewport" content="width=device-width"><link rel="stylesheet" href="/pure.css"><link rel="stylesheet" href="/style.css"><script defer src="/ui.js"></script><script defer src="/userpage.js"></script></head><body><div id="layout"><div id="main"><h1>Custom-page probe</h1><p id="message"></p><ul id="items"></ul><table id="rows"></table><form class="pure-form"><label>Value <input name="value"></label><label><input type="checkbox" name="enabled"> Enabled</label><button type="button" id='pump&amp;fan&quot;quoted' class="pure-button">Trigger</button></form></div></div></body></html>''')
 with FixtureServer(assets=assets) as server:
  server.fixtures['/api/v1/firmware'].update(ota_supported=True,reason=None)
  thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
  try:result=subprocess.run(['node',str(ROOT/'tests/test_ui_review.mjs'),'--base',f'http://127.0.0.1:{server.server_port}','--browser',args.browser,'--output',str(out)],timeout=120)
  finally:server.shutdown();thread.join(timeout=2)
raise SystemExit(result.returncode)
