#!/usr/bin/env python3
"""Exercise fixture failures and public-asset confinement over loopback."""
import _paths
import http.client
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from fixture_server import FixtureServer

ROOT = Path(__file__).resolve().parents[1]


class FixtureTests(unittest.TestCase):
    def setUp(self):
        base = ROOT / '.build-tests/fixtures'
        base.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        (root / 'public').mkdir()
        (root / 'public/index.html').write_text('Fixture application')
        (root / 'private.txt').write_text('synthetic private data')
        self.server = FixtureServer(assets=root / 'public')
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = 'http://127.0.0.1:' + str(self.server.server_port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.assertFalse(self.thread.is_alive())

    def get(self, route):
        with urllib.request.urlopen(self.base + route, timeout=2) as response:
            return response.read()

    def scenario(self, mode, route='/api/v1/firmware', **options):
        request = urllib.request.Request(self.base + '/__test/scenario',
            data=json.dumps({'route': route, 'mode': mode} | options).encode(),
            headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=2) as response:
            self.assertEqual(response.status, 200)

    def test_sdk_fixture_and_failure_modes(self):
        self.assertEqual(json.loads(self.get('/api/v1/firmware'))['sdk'], '3.0.6')
        self.scenario('http-error')
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.get('/api/v1/firmware')
        self.assertEqual(error.exception.code, 503)
        error.exception.close()
        self.scenario('malformed')
        with self.assertRaises(json.JSONDecodeError):
            json.loads(self.get('/api/v1/firmware'))
        self.scenario('disconnect')
        with self.assertRaises((http.client.RemoteDisconnected, urllib.error.URLError)):
            self.get('/api/v1/firmware')
        self.scenario('delayed', delay=0.15)
        started = time.monotonic()
        self.assertEqual(json.loads(self.get('/api/v1/firmware'))['version'], 'esp-serial2wifi v0.1.0')
        self.assertGreaterEqual(time.monotonic() - started, 0.12)
        self.scenario('normal')
        state = json.loads(self.get('/__test/state'))
        self.assertTrue(state['fixture'])
        self.assertEqual(state['requests']['/api/v1/firmware'], 5)

    def test_only_explicit_public_assets_are_served(self):
        self.assertEqual(self.get('/'), b'Fixture application')
        for route in ('/../private.txt', '/%2e%2e/private.txt', '/.git/config'):
            with self.subTest(route=route), self.assertRaises(urllib.error.HTTPError) as error:
                self.get(route)
            self.assertEqual(error.exception.code, 404)
            error.exception.close()
        request = urllib.request.Request(self.base + '/wifi/connect', data=b'fixture')
        with self.assertRaises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(request, timeout=2)
        self.assertEqual(error.exception.code, 405)
        error.exception.close()

    def test_root_and_retired_admin_routes(self):
        connection=http.client.HTTPConnection('127.0.0.1', self.server.server_port,timeout=2)
        connection.request('GET','/')
        response=connection.getresponse()
        self.assertEqual(response.status,302)
        self.assertEqual(response.getheader('Location'),'/index.html')
        response.read();connection.close()
        for route in ('/next/index.html','/network.html','/wifi','/console.html'):
            with self.subTest(route=route), self.assertRaises(urllib.error.HTTPError) as error:
                self.get(route)
            self.assertEqual(error.exception.code,404)
            error.exception.close()

    def test_redacted_versioned_status_and_feature_variants(self):
        status = json.loads(self.get('/api/v1/status'))
        caps = json.loads(self.get('/api/v1/capabilities'))
        self.assertEqual(status['schema'], 1)
        self.assertFalse(caps['features']['ota'])
        self.assertNotIn('password', json.dumps(status).lower())
        self.assertIsNone(status['serial']['target_connected'])
        status['wifi']['state'] = 'failed'
        self.scenario('normal', route='/api/v1/status', response=status)
        self.assertEqual(json.loads(self.get('/api/v1/status'))['wifi']['state'], 'failed')


if __name__ == '__main__':
    unittest.main(verbosity=2)
