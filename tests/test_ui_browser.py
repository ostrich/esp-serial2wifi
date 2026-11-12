#!/usr/bin/env python3
"""Real-time isolated browser tests against loopback-only replacement fixtures."""
import _paths
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from fixture_server import FixtureServer, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', required=True)
    parser.add_argument('--suite', choices=['all','management','updates','serial','network','branding'], default='all')
    parser.add_argument('--optimized',action='store_true')
    parser.add_argument('--external-ui',action='store_true')
    args = parser.parse_args()
    output = ROOT / '.build-tests/ui'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='public-', dir=output) as public:
        assets = Path(public)
        if args.optimized:
            subprocess.run([sys.executable,str(ROOT/'tools/build/build_ui.py'),'web','--output',str(assets),*(['--external-ui'] if args.external_ui else [])],check=True)
        else:
            subprocess.run([sys.executable,str(ROOT/'tools/build/build_ui.py'),'source','--output',str(assets)],check=True)
        with FixtureServer(assets=assets) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                result = subprocess.run(['node', str(ROOT / 'tests/test_ui_browser.mjs'),
                    '--base', f'http://127.0.0.1:{server.server_port}', '--browser', args.browser,
                    '--output', str(output), '--suite', args.suite], timeout=120)
            finally:
                server.shutdown()
                thread.join(timeout=2)
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
