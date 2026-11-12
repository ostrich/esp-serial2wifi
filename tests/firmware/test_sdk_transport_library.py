#!/usr/bin/env python3
"""Verify actual SDK object bindings with independent binutils and determinism."""

import _paths
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import prepare_sdk as library

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, required=True)
parser.add_argument("--ar", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
work = root / ".build-tests/sdk-library"
work.mkdir(parents=True, exist_ok=True)
archive = args.sdk / "lib/liblwip_536.a"
before = hashlib.sha256(archive.read_bytes()).hexdigest()
objdump = str(Path(args.ar).with_name("xtensa-lx106-elf-objdump"))
for member, names in library.OVERRIDES.items():
    data = subprocess.check_output([args.ar, "p", str(archive), member])
    original, output = work / ("original-" + member), work / member
    original.write_bytes(data)
    rewritten = library.rewrite(data, names)
    output.write_bytes(rewritten)
    symbols = subprocess.check_output(["readelf", "-Ws", str(output)], text=True)
    rows = {
        line.split()[-1]: line.split()
        for line in symbols.splitlines()
        if len(line.split()) == 8 and line.split()[0].endswith(":")
    }
    for name in names:
        weak, alias = rows[name], rows["vendor_" + name]
        assert weak[3:5] == ["FUNC", "WEAK"] and alias[3:5] == ["FUNC", "GLOBAL"]
        assert weak[1:3] == alias[1:3] and weak[6] == alias[6]

    def disassemble(path):
        text = subprocess.check_output([objdump, "-dr", str(path)], text=True)
        return text.replace(str(path), "OBJECT").replace("vendor_", "")

    assert disassemble(original) == disassemble(output), "instructions/relocations changed"
    for damaged in (data[:20], b"bad", rewritten):
        try:
            library.rewrite(damaged, names)
        except ValueError:
            pass
        else:
            raise AssertionError("malformed or already-rewritten object accepted")
    damaged = bytearray(data)
    struct.pack_into("<I", damaged, 32, 0xFFFFFFFF)
    try:
        library.rewrite(damaged, names)
    except ValueError:
        pass
    else:
        raise AssertionError("truncated section table accepted")
    try:
        library.rewrite(data, ("nonexistent_transport_function",))
    except ValueError:
        pass
    else:
        raise AssertionError("missing function accepted")

for name in ("one.a", "two.a"):
    subprocess.run(
        [
            sys.executable,
            str(root / "tools/build/prepare_sdk.py"),
            "--sdk",
            str(args.sdk),
            "--ar",
            args.ar,
            "--output",
            str(work / name),
        ],
        check=True,
    )
assert (work / "one.a").read_bytes() == (work / "two.a").read_bytes()
assert hashlib.sha256(archive.read_bytes()).hexdigest() == before == library.SDK_ARCHIVE_SHA256
print(
    "PASS actual SDK: independent instruction/relocation identity, weak/global aliases, invalid-input refusal, deterministic archives and untouched input"
)
