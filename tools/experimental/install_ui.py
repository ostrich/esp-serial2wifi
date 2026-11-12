#!/usr/bin/env python3
"""Back up, preflight and USB-install a 4MB banked-UI package. Never changes boot flags."""

import argparse, hashlib, json, os, struct, subprocess, sys, tempfile, re
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from flash_layout import layout, check_image
from ui_package import parse

ROOT = Path(__file__).resolve().parents[2]


def pages_used(flash):
    """Read existing custom-page ESPFS extents without treating corruption as free space."""
    start, end = 0x100000, 0x3FC000
    at = start
    if flash[start:end] == b"\xff" * (end - start):
        return 0
    while at + 16 <= end:
        magic, flags, compression, names, stored, plain = struct.unpack_from("<IBBHII", flash, at)
        if magic != 0x73665345 or compression != 0:
            raise ValueError(
                "Existing custom pages are unreadable; restore/export them before migrating"
            )
        if flags == 1:
            if names or stored or plain:
                raise ValueError("Malformed custom-page terminator")
            return at + 16 - start
        if (
            flags & ~2
            or names < 4
            or names > 256
            or names % 4
            or stored > 0x7FFFFFFF
            or plain > 0x7FFFFFFF
        ):
            raise ValueError("Malformed custom-page extent")
        next_at = at + 16 + names + (stored + 3) // 4 * 4
        if next_at > end or b"\0" not in flash[at + 16 : at + 16 + names]:
            raise ValueError("Custom pages extend beyond their region")
        at = next_at
    raise ValueError("Missing custom-page terminator")


def preflight(flash, package):
    if len(flash) != 0x400000:
        raise ValueError("A complete 4MB flash backup is required")
    slot, app, ui = parse(package)
    geometry = layout("4MB", True)
    if pages_used(flash) > geometry["regions"]["pages"][1]:
        raise ValueError("Existing custom pages overlap the reserved UI banks; no writes permitted")
    return slot, app, ui


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("package", type=Path)
    p.add_argument(
        "--backup",
        type=Path,
        help="Offline preflight against an existing full backup; never writes the device",
    )
    p.add_argument("--port")
    p.add_argument(
        "--active-slot",
        type=int,
        choices=(1, 2),
        help="Verify the currently running slot from device information before USB reset",
    )
    p.add_argument(
        "--target-disconnected",
        action="store_true",
        help="Confirm the attached device TX/RX wires are physically disconnected",
    )
    p.add_argument("--apply", action="store_true")
    p.add_argument("--backup-dir", type=Path, default=ROOT / "backups/ui-migration")
    p.add_argument("--esptool-python", type=Path, default=ROOT / ".venv/bin/python")
    a = p.parse_args()
    package = a.package.read_bytes()
    if a.backup:
        if a.apply:
            p.error("--backup is offline preflight only; installation always makes a fresh backup")
        slot, app, ui = preflight(a.backup.read_bytes(), package)
        print(
            f"Preflight passed: user{slot + 1}, app {len(app)} bytes, UI {len(ui)} bytes; no device access"
        )
        return
    if not a.port or not a.target_disconnected:
        p.error(
            "Device access requires --port and --target-disconnected (bootloader traffic must not reach the target)"
        )
    if a.apply and a.active_slot is None:
        p.error(
            "--apply requires the verified --active-slot; this installer preserves SDK boot flags"
        )
    a.backup_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    directory = a.backup_dir / stamp
    directory.mkdir(mode=0o700)
    command = [
        str(a.esptool_python),
        "-m",
        "esptool",
        "--chip",
        "esp8266",
        "--port",
        a.port,
        "--baud",
        "230400",
    ]

    def run(*args, first=False, reset=False):
        return subprocess.run(
            command
            + [
                "--before",
                "default-reset" if first else "no-reset",
                "--after",
                "hard-reset" if reset else "no-reset",
                *map(str, args),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    # First operation is a complete fresh backup. No flash writes precede it.
    backup = directory / "before.bin"
    run("read-flash", 0, 0x400000, backup, first=True)
    backup.chmod(0o600)
    flash = backup.read_bytes()
    info = run("flash-id").stdout
    if not re.search(r"Detected flash size:\s*4MB", info):
        raise ValueError("Device did not report a 4MB physical flash chip; no writes permitted")
    slot, app, ui = preflight(flash, package)
    with tempfile.TemporaryDirectory(dir=directory) as tmp:
        path = Path(tmp)
        application = path / "app.bin"
        application.write_bytes(app)
        check_image(application, layout("4MB", True), slot)
        if a.apply and not re.search(
            rb"ELSDK306-MAP4-CFG[12]", flash[0x1000 + slot * 0x80000 : 0x7C000 + slot * 0x80000]
        ):
            raise ValueError(
                "Active slot is not the same-SDK baseline; use the established SDK migration procedure"
            )
        if a.apply and slot != a.active_slot - 1:
            raise ValueError(
                "Package must match the verified active slot; serial installation does not change SDK slot flags"
            )
        (directory / "backup.json").write_text(
            json.dumps(
                {
                    "bytes": len(flash),
                    "sha256": hashlib.sha256(flash).hexdigest(),
                    "package_sha256": hashlib.sha256(package).hexdigest(),
                    "custom_pages_used": pages_used(flash),
                    "target_slot": slot + 1,
                },
                indent=2,
            )
            + "\n"
        )
        if not a.apply:
            run("read-flash", 0, 4, path / "boot-check.bin", reset=True)
            print(f"Fresh backup and preflight complete: {backup}; no flash writes performed")
            return
        bank = 0x37C000 + slot * 0x40000
        address = 0x1000 + slot * 0x80000
        ui_file = path / "ui.espfs"
        ui_file.write_bytes(ui)
        header = path / "ui.header"
        header.write_bytes(
            struct.pack("<8sI32sI", b"ELUI001\0", len(ui), hashlib.sha256(ui).digest(), 0)
        )

        def write_verified(at, file):
            run("write-flash", at, file)
            actual = path / "readback.bin"
            run("read-flash", at, file.stat().st_size, actual)
            if actual.read_bytes() != file.read_bytes():
                raise ValueError(
                    "Flash readback mismatch; leave in USB recovery and restore the full backup"
                )

        # Publish verified UI before changing the application. USB application
        # flashing itself is not atomic; preserve full backup and stable power.
        write_verified(bank + 4096, ui_file)
        write_verified(bank, header)
        write_verified(address, application)
        after = directory / "after.bin"
        run("read-flash", 0, 0x400000, after)
        after.chmod(0o600)
        actual = after.read_bytes()
        for left, right in [
            (0, address),
            (address + (len(app) + 4095) // 4096 * 4096, bank),
            (bank + (4096 + len(ui) + 4095) // 4096 * 4096, 0x400000),
        ]:
            if actual[left:right] != flash[left:right]:
                raise ValueError(
                    "Protected flash changed; leave in USB recovery and inspect the full backups"
                )
        # Final harmless read releases reset only after all verification passes.
        run("read-flash", 0, 4, path / "boot-check.bin", reset=True)
    print(
        f"Installed matching firmware/UI for user{slot + 1}; backups: {directory}. Verify the running version over Wi-Fi."
    )


if __name__ == "__main__":
    main()
