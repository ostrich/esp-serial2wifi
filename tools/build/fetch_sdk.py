#!/usr/bin/env python3
"""Fetch/extract checksum-pinned inputs, or verify them entirely offline."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def check_tree(archive, destination):
    with tarfile.open(archive) as tar:
        for member in tar:
            parts = Path(member.name).parts[1:]
            if not parts or not member.isfile():
                continue
            path = destination.joinpath(*parts)
            if not path.is_file():
                raise ValueError(f"missing pinned input: {path}")
            expected = hashlib.sha256(tar.extractfile(member).read()).digest()
            if hashlib.sha256(path.read_bytes()).digest() != expected:
                raise ValueError(f"modified pinned input: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--base", type=Path, default=ROOT / ".cache/esp8266")
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--compiler-dir", type=Path)
    args = parser.parse_args()
    lock = json.loads((ROOT / "sdk-lock.json").read_text())
    if args.sdk and args.sdk.resolve() != (args.base / lock["sdk"]["directory"]).resolve():
        parser.error("SDK_BASE must select the pinned SDK directory")
    if (
        args.compiler_dir
        and args.compiler_dir.resolve()
        != (args.base / lock["toolchain"]["directory"] / "bin").resolve()
    ):
        parser.error("XTENSA_TOOLS_ROOT must select the pinned compiler directory")
    args.base.mkdir(parents=True, exist_ok=True)
    for key, item in lock.items():
        archive = args.base / item["archive"]
        if not archive.exists():
            if args.offline or args.verify_only:
                parser.error(f"missing offline archive: {archive}")
            temporary = archive.with_suffix(archive.suffix + ".download")
            with (
                urllib.request.urlopen(item["url"], timeout=60) as source,
                temporary.open("wb") as dest,
            ):
                while chunk := source.read(1024 * 1024):
                    dest.write(chunk)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != item["sha256"]:
                parser.error(f"download checksum mismatch: {key}")
            os.replace(temporary, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != item["sha256"]:
            parser.error(f"archive checksum mismatch: {key}")
        destination = args.base / item["directory"]
        if not destination.exists():
            if args.verify_only:
                parser.error(f"missing extracted input: {destination}")
            with tempfile.TemporaryDirectory(
                prefix=destination.name + ".extract-", dir=args.base
            ) as staging:
                staging = Path(staging)
                with tarfile.open(archive) as tar:
                    tar.extractall(staging, filter="data")
                roots = list(staging.iterdir())
                if len(roots) != 1 or not roots[0].is_dir():
                    parser.error(f"unexpected archive root: {key}")
                check_tree(archive, roots[0])
                os.replace(roots[0], destination)
        check_tree(archive, destination)
        print(f"Verified pinned {key}: {item['version']}")


if __name__ == "__main__":
    main()
