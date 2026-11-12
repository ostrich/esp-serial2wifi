#!/usr/bin/env python3
"""Slot-specific, version-bound firmware/UI update packages; no device access."""

from pathlib import Path
import argparse, hashlib, struct
from flash_layout import layout, check_image, check_image_data

HEADER = 1024
MAGIC = b"ELUPD001"


def aligned(data):
    return data + b"\0" * (-len(data) % 1024)


def make(app, ui, slot):
    sha = hashlib.sha256(ui).digest()
    if b"ELUI-SHA256:" + sha.hex().encode() not in app:
        raise ValueError("Firmware and UI identities differ")
    if len(ui) > 0x3F000 or len(ui) < 16 or len(ui) % 4:
        raise ValueError("Invalid UI size")
    header = struct.pack("<8sIII32s", MAGIC, len(app), len(ui), slot, sha)
    return header + b"\0" * (HEADER - len(header)) + aligned(app) + aligned(ui)


def parse(data):
    if len(data) < HEADER:
        raise ValueError("Truncated package")
    magic, app_size, ui_size, slot, sha = struct.unpack_from("<8sIII32s", data)
    if (
        magic != MAGIC
        or slot > 1
        or any(data[52:HEADER])
        or not 1024 <= app_size <= 0x7B000
        or app_size % 16 != 4
        or not 16 <= ui_size <= 0x3F000
        or ui_size % 4
    ):
        raise ValueError("Invalid package header")
    ui_at = HEADER + (app_size + 1023) // 1024 * 1024
    if len(data) != ui_at + (ui_size + 1023) // 1024 * 1024:
        raise ValueError("Invalid package length")
    app = data[HEADER : HEADER + app_size]
    ui = data[ui_at : ui_at + ui_size]
    if any(data[HEADER + app_size : ui_at]) or any(data[ui_at + ui_size :]):
        raise ValueError("Invalid package padding")
    if hashlib.sha256(ui).digest() != sha or b"ELUI-SHA256:" + sha.hex().encode() not in app:
        raise ValueError("Firmware/UI identity mismatch")
    check_image_data(app, layout("4MB", True), slot)
    return slot, app, ui


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--ui", type=Path, required=True)
    a = p.parse_args()
    ui = a.ui.read_bytes()
    (a.directory / "ui.espfs").write_bytes(ui)
    for slot in range(2):
        path = a.directory / f"user{slot + 1}.bin"
        check_image(path, layout("4MB", True), slot)
        data = make(path.read_bytes(), ui, slot)
        parse(data)
        (a.directory / f"update-user{slot + 1}.elu").write_bytes(data)


if __name__ == "__main__":
    main()
