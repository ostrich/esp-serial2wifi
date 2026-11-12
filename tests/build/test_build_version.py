#!/usr/bin/env python3
"""Exercise the shared local/CI version policy in disposable Git checkouts."""

import _paths
import os, shutil, subprocess, tempfile
from pathlib import Path
from write_config import git_version

ROOT = Path(__file__).resolve().parents[2]
(ROOT / ".build-tests").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=ROOT / ".build-tests") as temporary:
    root = Path(temporary).resolve()

    def git(*args):
        return subprocess.check_output(
            ["git", "-C", str(root), *args], text=True, stderr=subprocess.DEVNULL
        ).strip()

    def rejected(**kwargs):
        try:
            git_version(root, **kwargs)
        except ValueError:
            return
        raise AssertionError("Invalid version request accepted")

    # An extracted source tree inside some other repository must not borrow its identity.
    rejected()
    git("init")
    git("config", "user.name", "Fixture")
    git("config", "user.email", "fixture@example.invalid")
    (root / ".gitignore").write_text("/build/\n")
    (root / "source.c").write_text("initial\n")
    git("add", ".")
    git("commit", "-m", "Initial")
    sha = git("rev-parse", "--short=8", "HEAD")
    assert git_version(root) == "dev." + sha
    git("tag", "v0.1.0")
    assert git_version(root) == "v0.1.0"
    assert git_version(root, development=True) == "v0.1.0-dev." + sha
    git("tag", "-a", "v0.2.0", "-m", "Annotated release")
    assert git_version(root) == "v0.2.0"
    for tag in ("v0.10.0", "v1.0.0-rc1", "master-build"):
        git("tag", tag)
    assert git_version(root) == "v1.0.0-rc1"
    assert git_version(root, development=True) == "v0.10.0-dev." + sha
    assert git_version(root, tag="v0.2.0") == "v0.2.0"
    rejected(tag="master-build")
    rejected(tag="v9.0.0")
    (root / "build").mkdir()
    (root / "build/output.bin").write_bytes(b"generated")
    assert not git_version(root).endswith("-dirty")
    (root / "new.c").write_text("new\n")
    assert git_version(root).endswith("-dirty")
    rejected(tag="v0.2.0")
    (root / "new.c").unlink()
    (root / "source.c").write_text("edited\n")
    assert git_version(root).endswith("-dirty")
    git("add", "source.c")
    assert git_version(root).endswith("-dirty")
    git("commit", "-m", "Development")
    sha = git("rev-parse", "--short=8", "HEAD")
    assert git_version(root) == "v0.10.0-dev." + sha
    rejected(tag="v0.2.0")
    head = git("rev-parse", "HEAD")
    git("checkout", "-b", "future")
    git("commit", "--allow-empty", "-m", "Unmerged")
    git("tag", "v9.0.0")
    git("checkout", "--detach", head)
    assert git_version(root) == "v0.10.0-dev." + sha
    # Real Make entry point: explicit versions win; no-Git builds fail; setup/help remain usable.
    archive = root / "build/archive"
    archive.mkdir()
    for name in ("Makefile", "mk/firmware.mk", "tools/build/write_config.py"):
        path = archive / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, path)
    for name in ("web/custom-pages", "web/app"):
        (archive / name).mkdir(parents=True)
    env = {
        k: v
        for k, v in os.environ.items()
        if k not in ("VERSION", "MAKEFLAGS", "MFLAGS", "MAKELEVEL", "SERIAL2WIFI_BUILD_CFLAGS")
    }

    def make(*args):
        return subprocess.run(
            ["make", "-s", *args], cwd=archive, env=env, text=True, capture_output=True
        )

    assert make("--eval=show-version:;@echo $(VERSION)", "show-version").returncode != 0
    result = make("VERSION=custom", "--eval=show-version:;@echo $(VERSION)", "show-version")
    assert result.returncode == 0 and result.stdout.strip() == "custom", result.stderr
    assert make("help").returncode == 0
print(
    "PASS local/CI versions: tags, numeric ordering, ancestry, overrides, dirty/ignored files, detached HEAD and missing Git metadata"
)
