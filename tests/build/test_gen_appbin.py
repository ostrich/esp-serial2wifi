#!/usr/bin/env python3
"""Binary-compatibility checks for the Python 3 SDK image generator.

Run through tests/run.py; vectors and synthetic sections are self-contained.
Temporary files stay within this workspace.
"""
import _paths

import binascii
import functools
import importlib.util
import json
import operator
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(os.environ.get("ESP_APPGEN_SCRIPT", ROOT / "tools/build/generate_appbin.py"))
TEMP = ROOT / ".build-tests/appbin"


class ImageTests(unittest.TestCase):
    def setUp(self):
        TEMP.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=TEMP)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.env = os.environ | {"COMPILE": "gcc"}
        binary_dir = self.work / "bin"
        binary_dir.mkdir()
        nm = binary_dir / "xtensa-lx106-elf-nm"
        nm.write_text(
            f"#!{sys.executable}\n"
            "import os,sys,json,pathlib\n"
            "pathlib.Path('nm-arguments.json').write_text(json.dumps(sys.argv[1:]))\n"
            "print(os.environ['NM_SYMBOLS'])\n"
            "sys.exit(int(os.environ.get('NM_EXIT', '0')))\n"
        )
        nm.chmod(0o755)
        self.env["PATH"] = str(binary_dir) + os.pathsep + self.env["PATH"]
        self.elf_name = "fixture with spaces;$(no-shell).out"
        self.sections = [b"\x00\x80\xff", b"hello", b"rodata\x00"]
        self.irom = b"irom\x00\xff"
        self.addresses = [0x40100000, 0x3FFE8000, 0x3FFE8C40]
        self.entry = 0x40100004

    def generate(self, mode=2, app=0, flash_mode=0, frequency=15, size=4, symbols=None):
        self.env["NM_SYMBOLS"] = symbols or (
            f"{self.entry:08x} T call_user_start\n"
            f"{self.addresses[1]:08x} A _data_start\n"
            f"{self.addresses[2]:08x} A _rodata_start\n"
        )
        for name, data in zip(("text", "data", "rodata"), self.sections):
            (self.work / f"eagle.app.v6.{name}.bin").write_bytes(data)
        (self.work / "eagle.app.v6.irom0text.bin").write_bytes(self.irom)
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                self.elf_name,
                str(mode),
                str(flash_mode),
                str(frequency),
                str(size),
                str(app),
            ],
            cwd=self.work,
            env=self.env,
            capture_output=True,
            text=True,
        )

    def test_sdk3_entry_wrapper_preferred(self):
        symbols = (f"{self.entry + 8:08x} T call_user_start\n"
                   f"{self.entry:08x} T call_user_start1\n"
                   f"{self.addresses[1]:08x} A _data_start\n"
                   f"{self.addresses[2]:08x} A _rodata_start\n")
        run = self.generate(symbols=symbols)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.image(), self.expected(2, 0))

    def image(self):
        return (self.work / "eagle.app.flash.bin").read_bytes()

    def expected(self, mode, app):
        def header(magic, count, flash_mode, size):
            return struct.pack("<BBBBI", magic, count, flash_mode, size, self.entry)

        data = bytearray()
        if mode == 2:
            data += header(0xEA, 4, 0, app)
            padded = self.irom + bytes(-len(self.irom) % 16)
            data += struct.pack("<II", 0, len(padded)) + padded
        data += header(0xE9, 3, 0, app if mode == 1 else 0x4F)
        checksum = 0xEF
        for address, section in zip(self.addresses, self.sections):
            padded = section + bytes(-len(section) % 4)
            data += struct.pack("<II", address, len(padded)) + padded
            checksum ^= functools.reduce(operator.xor, padded, 0)
        # Preserve the SDK's full padding line even at an aligned boundary.
        data += bytes(16 - (len(data) + 1) % 16) + bytes([checksum])
        if mode == 1:
            data += b"\xff" * max(0, 0x10000 - len(data)) + self.irom
        if mode in (1, 2):
            crc = binascii.crc32(data)
            footer = (~crc & 0xFFFFFFFF) if crc & 0x80000000 else crc + 1
            data += struct.pack("<I", footer)
        return bytes(data)

    def test_boot_modes_padding_and_both_crc_signs(self):
        signs = set()
        for mode in (0, 1, 2):
            for length in (1, 4, 15, 16, 17, 31):
                with self.subTest(mode=mode, length=length):
                    self.irom = bytes(range(length))
                    self.sections[0] = bytes(range(length))
                    process = self.generate(mode=mode, app=1)
                    self.assertEqual(process.returncode, 0, process.stderr)
                    self.assertEqual(self.image(), self.expected(mode, 1))
                    if mode in (1, 2):
                        signs.add(bool(binascii.crc32(self.image()[:-4]) & 0x80000000))
        self.assertEqual(signs, {False, True})

    def test_signed_crc_vectors(self):
        spec = importlib.util.spec_from_file_location("appbin", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        vector = self.work / "crc.bin"
        for data, expected in ((b"123456789", -873187034), (b"abc", 891568578)):
            vector.write_bytes(data)
            self.assertEqual(module.getFileCRC(vector), expected)

    def test_retry_overwrites_instead_of_appending(self):
        self.assertEqual(self.generate().returncode, 0)
        expected = self.image()
        self.assertEqual(self.generate().returncode, 0)
        self.assertEqual(self.image(), expected)

    def test_elf_path_is_passed_as_one_argument(self):
        self.assertEqual(self.generate().returncode, 0)
        self.assertEqual(
            json.loads((self.work / "nm-arguments.json").read_text()),
            ["-g", self.elf_name],
        )

    def test_nm_failure_propagates(self):
        self.env["NM_EXIT"] = "7"
        self.assertNotEqual(self.generate().returncode, 0)
        self.assertFalse((self.work / "eagle.app.flash.bin").exists())



if __name__ == "__main__":
    unittest.main(verbosity=2)
