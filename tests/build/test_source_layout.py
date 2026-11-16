#!/usr/bin/env python3
"""Check optional feature selection independently of source directory names."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def selection(modules):
    result = subprocess.check_output(
        [
            "make",
            "--no-print-directory",
            "-s",
            "VERSION=layout-test",
            f"MODULES={modules}",
            "--eval=layout-selection:;@echo $(SRC); echo $(CFLAGS)",
            "layout-selection",
        ],
        cwd=ROOT,
        text=True,
    ).splitlines()
    return set(result[0].split()), set(result[1].split())


base, flags = selection("")
assert any(p.startswith("src/httpd/") for p in base)
assert "src/api/routes.c" in base
features = {
    "mqtt": ("mqtt", "MQTT"),
    "rest": ("http_client", "REST"),
    "socket": ("socket", "SOCKET"),
    "web-server": ("custom_pages", "WEBSERVER"),
    "syslog": ("syslog", "SYSLOG"),
}
all_sources = set(base)
for feature, (directory, define) in features.items():
    expected = {str(p.relative_to(ROOT)) for p in (ROOT / "src" / directory).glob("*.c")}
    enabled, flags = selection(feature)
    assert enabled == base | expected, feature
    assert f"-D{define}" in flags, feature
    assert not base & expected, feature
    all_sources |= expected
combined, flags = selection(" ".join(features))
assert combined == all_sources == {str(p.relative_to(ROOT)) for p in (ROOT / "src").glob("*/*.c")}
print(
    "PASS source layout: independent optional features, always-on HTTP and complete source coverage"
)
