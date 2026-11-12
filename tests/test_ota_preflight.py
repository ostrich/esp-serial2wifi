#!/usr/bin/env python3
"""Read-only CLI migration policy using synthetic server metadata and files."""
import _paths
from pathlib import Path
import tempfile
import unittest
from flash_ota import validate, RUNTIME_VERSION

ROOT = Path(__file__).resolve().parents[1]


class PreflightTests(unittest.TestCase):
    def test_supported_target_and_rejected_migrations(self):
        base = ROOT / '.build-tests/preflight'
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as name:
            files = [Path(name) / f'user{slot+1}.bin' for slot in range(2)]
            for slot, file in enumerate(files):
                file.write_bytes(bytes((0xea, 4, 0, slot)) + b'ELSDK306-MAP4-CFG1')
            good = {'active_slot': 1, 'target_slot': 2, 'sdk': RUNTIME_VERSION, 'ota_supported': True, 'image_abi': 'ELSDK306-MAP4-CFG1'}
            validate(good, files)
            for patch in ({'sdk': '2.1.0'}, {'sdk': '3.0.60'}, {'sdk': ''},
                          {'active_slot': True}, {'target_slot': True}, {'image_abi': None}, {'active_slot': None}, {'target_slot': 1}, {'ota_supported': False}, {'ota_supported': 'true'},
                          {'image_abi': 'ELSDK306-MAP2-CFG1'}):
                with self.subTest(patch=patch), self.assertRaises(ValueError):
                    validate(good | patch, files)
            with self.assertRaises(ValueError):
                validate(good, files[::-1])
            files[0].write_bytes(b'legacy image without SDK identity')
            with self.assertRaises(ValueError):
                validate(good, files)


if __name__ == '__main__':
    unittest.main(verbosity=2)
