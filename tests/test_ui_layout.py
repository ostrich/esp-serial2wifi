#!/usr/bin/env python3
"""Packaged all-screen layout checks against loopback-only fixtures."""
import _paths
import argparse,subprocess,sys,tempfile,threading
from pathlib import Path
from fixture_server import FixtureServer,ROOT
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--browser',required=True);a=p.parse_args()
out=ROOT/'.build-tests/layout-ui';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(dir=out) as tmp:
 assets=Path(tmp)
 subprocess.run([sys.executable,str(ROOT/'tools/build/build_ui.py'),'web','--output',str(assets)],check=True)
 with FixtureServer(assets=assets) as server:
  t=threading.Thread(target=server.serve_forever,daemon=True);t.start()
  try:r=subprocess.run(['node',str(ROOT/'tests/test_ui_layout.mjs'),'--base',f'http://127.0.0.1:{server.server_port}','--browser',a.browser,'--output',str(out)],timeout=180)
  finally:server.shutdown();t.join(timeout=2)
raise SystemExit(r.returncode)
