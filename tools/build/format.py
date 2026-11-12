#!/usr/bin/env python3
"""Apply or check pinned formatters on maintained repository files."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    paths = (
        subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT
        )
        .decode()
        .split("\0")
    )
    paths = sorted(
        set(p for p in paths if p and (ROOT / p).is_file() and not p.startswith("third_party/"))
    )
    bins = ROOT / ".venv/bin"
    groups = [
        (
            [str(bins / "clang-format"), *(["--dry-run", "--Werror"] if args.check else ["-i"])],
            {".c", ".h"},
        ),
        ([str(bins / "ruff"), "format", *(["--check"] if args.check else [])], {".py"}),
        (
            [str(ROOT / "node_modules/.bin/prettier"), "--check" if args.check else "--write"],
            {".js", ".mjs", ".css", ".html", ".json", ".yml", ".yaml", ".md"},
        ),
        ([str(bins / "shfmt"), "-i", "2", "-d" if args.check else "-w"], {".sh"}),
    ]
    failed = False
    for command, extensions in groups:
        files = [
            p
            for p in paths
            if Path(p).suffix in extensions
            and p not in ("web/custom-pages/pure.css", "package-lock.json")
        ]
        if files:
            result = subprocess.run([*command, *files], cwd=ROOT)
            failed |= result.returncode != 0
    # Make recipes retain literal tabs; apply common text hygiene to build files.
    for name in ("Makefile", "mk/firmware.mk", "config.mk.example"):
        path = ROOT / name
        text = path.read_text()
        clean = "\n".join(line.rstrip() for line in text.splitlines()) + "\n"
        if clean != text:
            if args.check:
                print(f"{name}: trailing whitespace or missing final newline", file=sys.stderr)
                failed = True
            else:
                path.write_text(clean)
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
