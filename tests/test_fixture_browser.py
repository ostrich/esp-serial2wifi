#!/usr/bin/env python3
"""Exercise loopback fixtures in an isolated real headless Chromium browser.

No running browser session or device is used. All browser files remain in the
source tree's ignored .build-tests directory. Requires an existing Chromium
family executable; pass --browser to select it explicitly.
"""
import _paths
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading

from fixture_server import FixtureServer, ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', default=next((found for name in
                        ('chromium', 'chromium-browser', 'brave', 'google-chrome-stable')
                        if (found := shutil.which(name))), None))
    args = parser.parse_args()
    if not args.browser:
        parser.error('no existing Chromium family browser found; use --browser')
    output = ROOT / '.build-tests'
    output.mkdir(exist_ok=True)
    server = FixtureServer(assets=ROOT / 'tests/fixtures/browser')
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='fixture-browser-', dir=output) as profile:
            command = [args.browser, '--headless', '--disable-gpu', '--no-first-run',
                       '--no-default-browser-check', '--disable-background-networking',
                       '--disable-component-update', '--disable-extensions',
                       '--user-data-dir=' + str(Path(profile).resolve()),
                       '--virtual-time-budget=10000', '--timeout=15000', '--dump-dom',
                       f'http://127.0.0.1:{server.server_port}/index.html']
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            except subprocess.TimeoutExpired as error:
                (output / 'fixture-browser.stderr').write_bytes(error.stderr or b'')
                raise SystemExit('Browser fixture launch timed out; see .build-tests/fixture-browser.stderr')
            (output / 'fixture-browser.html').write_text(result.stdout)
            (output / 'fixture-browser.stderr').write_text(result.stderr)
            if result.returncode or 'data-result="pass"' not in result.stdout:
                raise SystemExit('Browser fixture check failed; see .build-tests/fixture-browser.*')
            print('PASS: real browser normal/error/malformed/delay/cancel/disconnect/recovery and mutation refusal')
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == '__main__':
    main()
