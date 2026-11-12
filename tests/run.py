#!/usr/bin/env python3
"""Run host, browser, layout or experimental checks, or one test relative to tests/."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'
SUPPORT = TESTS / 'support'
sys.path.insert(0, str(SUPPORT))
ENV = {**os.environ, 'PYTHONPATH': str(SUPPORT)}


def command(args, timeout=180):
    subprocess.run(list(map(str, args)), cwd=ROOT, env=ENV, check=True, timeout=timeout)


def test(path, *args):
    path = (TESTS / path).resolve()
    if not path.is_relative_to(TESTS) or not path.is_file():
        raise ValueError('Select an existing test inside tests/')
    if path == TESTS/'web/test_status_model.mjs':
        # Also honor this dependency when running the model test on its own.
        test('firmware/test_api_status.py')
    print(f'\n>>> {path.relative_to(ROOT)}', flush=True)
    command(['node' if path.suffix == '.mjs' else sys.executable, path, *args])


def host(args):
    test('web/test_status_model.mjs')
    special = {'test_api_status.py', 'test_sdk_rto.py', 'test_sdk_credit.py',
               'test_sdk_transport_library.py', 'test_firmware_image.py', 'test_ota.py'}
    for group in ('build', 'firmware', 'web'):
        for path in sorted((TESTS/group).glob('test_*.py')):
            if path.name not in special and path.name != 'test_release_archive.py':
                test(path)
    for path in sorted((TESTS/'web').glob('test_*.mjs')):
        if path.name.endswith('_model.mjs') or path.name in ('test_client.mjs', 'test_custom_page_lifecycle.mjs'):
            if path.name != 'test_status_model.mjs':
                test(path)
    for name in ('rto', 'credit', 'transport_library'):
        extra = ['--ar', args.ar] if name == 'transport_library' else []
        test(f'firmware/test_sdk_{name}.py', '--sdk', args.sdk, *extra)
    output = ROOT / '.build-tests/test-uart'
    output.parent.mkdir(exist_ok=True)
    command(['gcc', '-std=c99', '-Wall', '-Wextra', '-Wno-unused-parameter',
             '-fsanitize=address,undefined', '-Itests/support/uart-test-stubs',
             '-Iinclude', '-Isrc/core', '-Isrc/serial', 'tests/firmware/test_uart_driver.c', '-o', output])
    command([output])
    if args.flash == '4MB':
        test('firmware/test_firmware_image.py')
        test('firmware/test_ota.py')


def browser(args):
    from fixture_server import FixtureServer
    output = ROOT / '.build-tests' / ('layout-ui' if args.mode == 'layout' else 'ui')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='assets-', dir=output) as directory:
        assets = Path(directory)
        command([sys.executable, 'tools/build/build_ui.py', 'source' if args.source else 'web',
                 '--output', assets, *(['--external-ui'] if args.external_ui else [])])
        if args.source:
            for name in ('ui.js', 'userpage.js', 'pure.css', 'style.css'):
                shutil.copyfile(ROOT/'web/custom-pages'/name, assets/('compat-style.css' if name == 'style.css' else name))
        (assets/'user').mkdir(exist_ok=True)
        shutil.copyfile(SUPPORT/'fixtures/custom-page.html', assets/'user/probe.html')
        if args.source:
            page=assets/'user/probe.html'
            page.write_text(page.read_text().replace('href="/style.css"', 'href="/compat-style.css"'))
        suites = ['layout'] if args.mode == 'layout' else (['app', 'recovery', 'clipboard'] if args.suite == 'all' else [args.suite])
        for suite in suites:
            out = output/suite
            out.mkdir(exist_ok=True)
            params = ['--browser', args.browser, '--output', out]
            if suite == 'clipboard':
                test('web/test_clipboard_browser.mjs', *params)
                continue
            with FixtureServer(assets=assets) as server:
                if suite == 'recovery':
                    server.fixtures['/api/v1/firmware'].update(ota_supported=True, reason=None)
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                try:
                    feature = suite in ('management', 'updates', 'serial', 'network', 'branding')
                    test(f'web/test_{"app" if feature else suite}_browser.mjs',
                         '--base', f'http://127.0.0.1:{server.server_port}', *params,
                         *(['--suite', suite] if feature else []))
                finally:
                    server.shutdown()
                    thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', help='host, browser, layout, experimental, or a test path')
    # Individual tests own their arguments (including --help).
    if len(sys.argv)>1 and sys.argv[1].endswith(('.py', '.mjs')):
        test(sys.argv[1], *sys.argv[2:])
        return
    parser.add_argument('--sdk', default=str(ROOT/'.cache/esp8266/ESP8266_NONOS_SDK-3.0.6'))
    parser.add_argument('--ar', default=str(ROOT/'.cache/esp8266/xtensa-lx106-elf/bin/xtensa-lx106-elf-ar'))
    parser.add_argument('--flash', choices=['512KB', '4MB'], default='4MB')
    parser.add_argument('--browser')
    parser.add_argument('--suite', choices=['all','app','recovery','clipboard','management','updates','serial','network','branding'], default='all')
    parser.add_argument('--source', action='store_true', help='test unbundled UI instead of packaged assets')
    parser.add_argument('--external-ui', action='store_true')
    args = parser.parse_args()
    if args.mode == 'host':
        host(args)
    elif args.mode in ('browser', 'layout'):
        if not args.browser:
            parser.error('--browser is required')
        browser(args)
    elif args.mode == 'experimental':
        test('experimental/test_ui_package.py')
        test('experimental/test_ui_storage.py')
    else:
        parser.error('unknown suite')


if __name__ == '__main__':
    main()
