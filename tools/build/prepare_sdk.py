#!/usr/bin/env python3
"""Add overridable TCP timer/retry bindings without rebuilding SDK structures.

Only ELF symbol/string tables change. Vendor instructions, data and relocation
indices stay intact. The corresponding application shim calls the added aliases.
The pinned SDK's public espconn_msg tail differs from its shipped ABI; rebuilding
whole SDK objects from those headers would be unsafe.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

OVERRIDES = {"tcp.o": ("tcp_slowtmr",), "tcp_out.o": ("tcp_rexmit_rto", "tcp_rexmit_fast")}
SDK_ARCHIVE_SHA256 = "b478f135c36d7c58269d84100e9c8233a2533c076e754f926e690eaa64c34536"


class Elf32:
    def __init__(self, data):
        self.data = bytearray(data)
        if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
            raise ValueError("expected little-endian ELF32")
        if struct.unpack_from("<HH", data, 16) != (1, 94):
            raise ValueError("expected relocatable Xtensa object")
        self.shoff = struct.unpack_from("<I", data, 32)[0]
        self.shsize, count, self.names_index = struct.unpack_from("<HHH", data, 46)
        if self.shsize != 40 or not count or self.names_index >= count:
            raise ValueError("unsupported section table")
        self.slice(self.shoff, count * self.shsize)
        self.headers = [
            list(struct.unpack_from("<IIIIIIIIII", data, self.shoff + i * 40)) for i in range(count)
        ]
        for h in self.headers:
            if h[1] != 8:
                self.slice(h[4], h[5])

    def slice(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError("truncated ELF section")
        return self.data[offset : offset + size]

    @staticmethod
    def string(table, offset):
        end = table.find(b"\0", offset)
        if offset >= len(table) or end < 0:
            raise ValueError("invalid ELF string")
        return bytes(table[offset:end]).decode("ascii")

    def allocated(self):
        names = self.headers[self.names_index]
        table = self.slice(names[4], names[5])
        return [
            (
                i,
                self.string(table, h[0]),
                h[1:4],
                h[5],
                bytes(self.slice(h[4], h[5])) if h[1] != 8 else b"",
            )
            for i, h in enumerate(self.headers)
            if h[2] & 2
        ]

    def add_overrides(self, names):
        indices = [i for i, h in enumerate(self.headers) if h[1] == 2]
        if len(indices) != 1:
            raise ValueError("expected one symbol table")
        si = indices[0]
        h = self.headers[si]
        if h[9] != 16 or h[5] % 16 or h[6] >= len(self.headers):
            raise ValueError("unsupported symbol table")
        strings_index = h[6]
        st = self.headers[strings_index]
        if st[1] != 3:
            raise ValueError("invalid symbol string table")
        strings = self.slice(st[4], st[5])
        symbols = self.slice(h[4], h[5])
        entries = [
            list(struct.unpack_from("<IIIBBH", symbols, i)) for i in range(0, len(symbols), 16)
        ]
        for name in names:
            selected = [(i, e) for i, e in enumerate(entries) if self.string(strings, e[0]) == name]
            alias = "vendor_" + name
            if len(selected) != 1 or any(self.string(strings, e[0]) == alias for e in entries):
                raise ValueError("missing/ambiguous symbol or existing alias: " + name)
            index, e = selected[0]
            if e[3] != 0x12 or not 0 < e[5] < len(self.headers):
                raise ValueError("expected defined global function: " + name)
            e[3] = 0x22  # STB_WEAK | STT_FUNC, preserving its original index.
            struct.pack_into("<IIIBBH", symbols, index * 16, *e)
            added = e.copy()
            added[0], added[3] = len(strings), 0x12
            strings.extend(alias.encode("ascii") + b"\0")
            symbols.extend(struct.pack("<IIIBBH", *added))
        while len(self.data) % 4:
            self.data.append(0)
        h[4], h[5] = len(self.data), len(symbols)
        self.data.extend(symbols)
        st[4], st[5] = len(self.data), len(strings)
        self.data.extend(strings)
        for index in (si, strings_index):
            struct.pack_into(
                "<IIIIIIIIII", self.data, self.shoff + index * 40, *self.headers[index]
            )
        return bytes(self.data)


def rewrite(data, names):
    original = Elf32(data)
    allocated = original.allocated()
    rewritten = original.add_overrides(names)
    if Elf32(rewritten).allocated() != allocated:
        raise ValueError("vendor allocated section changed")
    return rewritten


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--ar", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.sdk / "lib/liblwip_536.a").resolve()
    output = args.output.resolve()
    if source == output:
        parser.error("refusing to modify SDK input archive")
    output.parent.mkdir(parents=True, exist_ok=True)
    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    if source_hash != SDK_ARCHIVE_SHA256:
        parser.error("transport bindings require the pinned SDK3.0.6 liblwip_536.a")
    with tempfile.TemporaryDirectory(prefix=".transport-", dir=output.parent) as directory:
        work = Path(directory)
        subprocess.run([args.ar, "x", str(source), *OVERRIDES], cwd=work, check=True)
        for member, names in OVERRIDES.items():
            p = work / member
            p.write_bytes(rewrite(p.read_bytes(), names))
        temporary = work / output.name
        shutil.copyfile(source, temporary)
        subprocess.run(
            [args.ar, "rD", str(temporary), *(str(work / member) for member in OVERRIDES)],
            check=True,
        )
        temporary.replace(output)
    if hashlib.sha256(source.read_bytes()).hexdigest() != source_hash:
        raise ValueError("SDK source changed during preparation")
    manifest = {
        "source_sha256": source_hash,
        "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "allocated_vendor_sections_unchanged": True,
        "overrides": OVERRIDES,
    }
    output.with_suffix(".json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
