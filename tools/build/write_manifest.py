#!/usr/bin/env python3
"""Record validated geometry, pinned inputs and artifact identity; no boot claims."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
import flash_layout
from prepare_sdk import OVERRIDES, SDK_ARCHIVE_SHA256

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=flash_layout.TARGETS)
    parser.add_argument("--external-ui", action="store_true")
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--debug", type=int, required=True)
    parser.add_argument("--ota-enabled", type=int, choices=(0, 1), default=0)
    parser.add_argument("--ota-testing", type=int, required=True)
    parser.add_argument("--modules", required=True)
    parser.add_argument("--mode", type=int, required=True)
    parser.add_argument("--frequency", type=int, required=True)
    args = parser.parse_args()
    if args.mode not in (0, 1, 2, 3) or args.frequency not in (0, 1, 2, 15):
        parser.error("unsupported SPI mode/frequency code")
    geometry = flash_layout.layout(args.target, args.external_ui)
    nm = args.sdk.parent / "xtensa-lx106-elf/bin/xtensa-lx106-elf-nm"
    symbols = subprocess.check_output(
        [str(nm), "-g", str(ROOT / "build/esp-serial2wifi.user1.elf")], text=True
    )
    linked_slots = []
    for slot in range(2 if geometry["ota_geometry"] else 1):
        slot_symbols = (
            symbols
            if slot == 0
            else subprocess.check_output(
                [str(nm), "-g", str(ROOT / f"build/esp-serial2wifi.user{slot + 1}.elf")], text=True
            )
        )
        rows = (line.split() for line in slot_symbols.splitlines())
        named = {parts[2]: (parts[0], parts[1]) for parts in rows if len(parts) == 3}
        for names in OVERRIDES.values():
            for name in names:
                replacement = named.get(name)
                vendor = named.get("vendor_" + name)
                if (
                    not replacement
                    or not vendor
                    or replacement[1] != "T"
                    or vendor[1] != "T"
                    or replacement[0] == vendor[0]
                ):
                    raise SystemExit(
                        f"user{slot + 1}: SDK transport override/alias not linked: {name}"
                    )
        linked_slots.append(slot + 1)
    transport = json.loads((ROOT / "build/sdk-transport/liblwip_536.json").read_text())
    if (
        transport["source_sha256"] != SDK_ARCHIVE_SHA256
        or not transport["allocated_vendor_sections_unchanged"]
    ):
        raise SystemExit("SDK transport input/section verification missing")
    if digest(ROOT / "build/sdk-transport/liblwip_536.a") != transport["output_sha256"]:
        raise SystemExit("SDK transport shadow archive differs from its verified manifest")
    transport["linked_slots"] = linked_slots
    transport["policy"] = {
        "port": 23,
        "mss_for_credit_increase": 536,
        "initial_send_credit_bytes": 2144,
        "minimum_measured_rto_us": 200000,
        "maximum_measured_rto_us": 60000000,
        "initial_vendor_timeout_unchanged": True,
        "source_block_release": "after-verified-sdk-copy-or-ack",
        "logical_queue_limit_bytes": 12864,
        "maximum_application_flights": 4,
    }
    has_patch = any(line.endswith(" patch_data") for line in symbols.splitlines())
    if has_patch != (args.target == "2MB"):
        parser.error(
            "startup link selection did not preserve the capacity-specific vendor patch policy"
        )
    images = {}
    for slot in range(2 if geometry["ota_geometry"] else 1):
        path = ROOT / f"firmware/user{slot + 1}.bin"
        flash_layout.check_image(path, geometry, slot)
        data = path.read_bytes()
        inner = 16 + struct.unpack_from("<I", data, 12)[0]
        if data[inner + 2] != args.mode or data[inner + 3] & 15 != args.frequency:
            parser.error("image SPI header does not match the build manifest")
        images[path.name] = {
            "address": geometry["regions"][f"app{slot + 1}"][0],
            "bytes": path.stat().st_size,
            "sha256": digest(path),
        }
    if args.external_ui:
        for slot in range(2):
            path = ROOT / f"firmware/update-user{slot + 1}.elu"
            images[path.name] = {
                "bytes": path.stat().st_size,
                "sha256": digest(path),
                "format": "ELUPD001",
                "target_slot": slot + 1,
            }
        path = ROOT / "build/ui.espfs"
        images["ui.espfs"] = {
            "bytes": path.stat().st_size,
            "sha256": digest(path),
            "format": "ESPFS",
        }
    system = {}
    for name in ("boot_v1.6.bin", "esp_init_data_default_v05.bin", "blank.bin"):
        path = args.sdk / "bin" / name
        system[name] = {"bytes": path.stat().st_size, "sha256": digest(path)}
    result = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True
    )
    revision = result.stdout.strip() if result.returncode == 0 else None
    names = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        capture_output=True,
    )
    tree_hash = None
    if names.returncode == 0:
        checksum = hashlib.sha256()
        for name in sorted(set(names.stdout.split(b"\0")) - {b""}):
            path = ROOT / name.decode()
            if path.is_file():
                checksum.update(name + b"\0" + hashlib.sha256(path.read_bytes()).digest())
        tree_hash = checksum.hexdigest()
    manifest = {
        "schema": 1,
        "version": args.version,
        "source_revision": revision,
        "source_tree_sha256": tree_hash,
        "inputs": json.loads((ROOT / "sdk-lock.json").read_text()),
        "sdk_transport": transport,
        "ui": {
            "storage": "slot-banked" if args.external_ui else "embedded",
            "terminal_version": "6.0.0" if args.external_ui else "5.5.0",
        },
        "geometry": geometry,
        "images": images,
        "system_images": system,
        "spi_mode": args.mode,
        "spi_frequency_code": args.frequency,
        "modules": args.modules.split(),
        "debug": bool(args.debug),
        "ota": (
            "explicit-validation-build"
            if args.ota_testing
            else "enabled-sdk306-4mb"
            if args.ota_enabled and args.target == "4MB"
            else "disabled"
        ),
        "cross_sdk_ota": "unsupported-use-serial",
        "hardware_validation": "not-established-by-build",
    }
    (ROOT / "firmware/release-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )


if __name__ == "__main__":
    main()
