#!/usr/bin/env python3
"""Local browser regressions for frontend operation ownership and recovery."""
import _paths
import argparse,subprocess,tempfile,threading
from pathlib import Path
from fixture_server import FixtureServer,ROOT
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--browser',required=True)
p.add_argument('--optimized',action='store_true')
a=p.parse_args();out=ROOT/'.build-tests/ui-lifecycle'/('optimized' if a.optimized else 'source');out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as tmp:
 assets=Path(tmp)
 subprocess.run(['python3',str(ROOT/'tools/build/build_ui.py'),'web' if a.optimized else 'source','--output',str(assets)],check=True)
 # Retained custom pages are also covered when testing unbundled modules.
 if not a.optimized:
  import shutil
  for name in ('ui.js','userpage.js','pure.css','style.css'):shutil.copyfile(ROOT/'web/custom-pages'/name,assets/name if name!='style.css' else assets/'compat-style.css')
 (assets/'user').mkdir()
 (assets/'user/probe.html').write_text('<!doctype html><script defer src="/ui.js"></script><script defer src="/userpage.js"></script><meta name="refresh-rate" content="0"><div id="layout"><div id="main"><p id="value"></p><form><input type="checkbox" name="enabled"><button type="submit">Save</button></form></div></div>')
 with FixtureServer(assets=assets) as server:
  t=threading.Thread(target=server.serve_forever,daemon=True);t.start()
  try:r=subprocess.run(['node',str(ROOT/'tests/test_ui_lifecycle.mjs'),'--base',f'http://127.0.0.1:{server.server_port}','--browser',a.browser,'--output',str(out)],timeout=120)
  finally:server.shutdown();t.join(timeout=2)
raise SystemExit(r.returncode)
