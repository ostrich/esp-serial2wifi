#!/usr/bin/env python3
"""Prepare repository-local dependencies. Does not access attached devices."""

import argparse, platform, re, shutil, subprocess, sys, venv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--offline", action="store_true")
    p.add_argument("--base", type=Path, default=ROOT / ".cache/esp8266")
    a = p.parse_args()
    if sys.version_info < (3, 12):
        p.error("Python 3.12 or newer is required")
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
        p.error("The pinned compiler requires x86-64 Linux")
    for command in ("make", "cc", "gcc", "readelf", "nm", "ar", "tar", "node", "npm", "sed"):
        if not shutil.which(command):
            p.error("Install the host prerequisite: " + command + " (see docs/development.md)")
    version = re.search(
        r"GNU Make (\d+)\.(\d+)", subprocess.check_output(["make", "--version"], text=True)
    )
    if not version or tuple(map(int, version.groups())) < (4, 3):
        p.error("GNU Make 4.3 or newer is required")
    if (
        int(
            subprocess.check_output(["node", "--version"], text=True)
            .strip()
            .lstrip("v")
            .split(".")[0]
        )
        < 22
    ):
        p.error("Node.js 22 or newer is required")
    folder = ROOT / ".venv"
    python = folder / "bin/python"
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(folder)
    subprocess.run(
        [
            str(python),
            "-m",
            "pip",
            "install",
            *(["--no-index"] if a.offline else []),
            "-r",
            str(ROOT / "requirements-build.txt"),
        ],
        check=True,
        cwd=ROOT,
    )
    subprocess.run(["npm", "ci", *(["--offline"] if a.offline else [])], check=True, cwd=ROOT)
    subprocess.run(
        [
            str(python),
            str(ROOT / "tools/build/fetch_sdk.py"),
            "--base",
            str(a.base),
            *(["--offline"] if a.offline else []),
        ],
        check=True,
        cwd=ROOT,
    )
    print("Ready. Run make, or make FLASH_SIZE=512KB. Outputs: firmware/.")


if __name__ == "__main__":
    main()
