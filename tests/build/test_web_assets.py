#!/usr/bin/env python3
"""Packaged asset identity, allowlist, licenses and HTTP product headers."""

import _paths
from source_text import SourceText
from pathlib import Path
import ast, hashlib, json, re, struct, subprocess, sys, tempfile

ROOT = Path(__file__).resolve().parents[2]
base = ROOT / ".build-tests/web-assets"
base.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=base) as tmp:
    output = Path(tmp) / "public"
    output.mkdir()
    (output / "console.js").write_text("stale admin code")
    subprocess.run(
        [sys.executable, str(ROOT / "tools/build/build_ui.py"), "web", "--output", str(output)],
        check=True,
    )
    version = re.search(
        r'src="([a-f0-9]{24})/app.js"', SourceText((output / "index.html").read_text())
    ).group(1)
    digest = hashlib.sha256()
    for file in sorted((output / version).iterdir()):
        digest.update(file.name.encode() + b"\0")
        digest.update(file.read_bytes())
    assert digest.hexdigest()[:24] == version, "Asset identity must cover optimized bytes"
    expected = {"favicon.ico", "pure.css", "style.css", "ui.js", "userpage.js", "index.html"} | {
        version + "/" + name for name in ("style.css", "xterm.css", "app.js", "xterm.js")
    }
    actual = {p.relative_to(output).as_posix() for p in output.rglob("*") if p.is_file()}
    assert actual == expected, (actual - expected, expected - actual)
    assert b"Permission is hereby granted" in (output / version / "xterm.js").read_bytes()
print(
    "PASS production asset allowlist: replacement UI, compatibility helpers, no stale/admin assets"
)


OUT = ROOT / ".build-tests/branding"
OUT.mkdir(parents=True, exist_ok=True)
recovery = next(
    ast.literal_eval(n.value)
    for n in ast.parse(SourceText((ROOT / "tools/build/build_ui.py").read_text())).body
    if isinstance(n, ast.Assign)
    and any(isinstance(t, ast.Name) and t.id == "RECOVERY" for t in n.targets)
)
assert "ESP Serial2WiFi recovery" in recovery and "X-Serial2WiFi-Request" in recovery
assert (
    "esp-serial2wifi-assets"
    == json.loads(SourceText((ROOT / "package.json").read_text()))["name"]
    == json.loads(SourceText((ROOT / "package-lock.json").read_text()))["packages"][""]["name"]
)
icon = (ROOT / "web/custom-pages/favicon.ico").read_bytes()
assert struct.unpack("<HHH", icon[:6]) == (0, 1, 2)
assert {(icon[6 + 16 * i], icon[7 + 16 * i]) for i in range(2)} == {(16, 16), (32, 32)}
# Exercise real HTTP headers and MIME selection.
source = SourceText((ROOT / "src/httpd/httpd.c").read_text())
mime = source[
    source.index("typedef struct {\n  const char *ext;") : source.index(
        "// debug string to identify connection"
    )
]
response = source[
    source.index("void ICACHE_FLASH_ATTR httpdStartResponse(") : source.index(
        "//Use this as a cgi function"
    )
]
code = (
    r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ICACHE_FLASH_ATTR
#define os_strcmp strcmp
#define os_sprintf sprintf
typedef struct {int code;} Priv;
typedef struct {Priv *priv;} HttpdConnData;
static char output[2048];
static int httpdSend(HttpdConnData *c,const char *s,int n){(void)c;assert(strlen(output)+n<sizeof output);strncat(output,s,n);return 1;}
"""
    + mime
    + response
    + r"""
int main(void){Priv p;HttpdConnData c={&p};
 httpdStartResponse(&c,200);assert(strstr(output,"Server: esp-serial2wifi\r\n"));assert(p.code==200);
 output[0]=0;httpdRedirect(&c,"/index.html");assert(strstr(output,"Server: esp-serial2wifi\r\n"));assert(strstr(output,"Location: /index.html\r\n"));assert(p.code==302);
 assert(!strcmp(httpdGetMimetype("/favicon.ico"),"image/x-icon"));
 puts("PASS native HTTP response/redirect product headers and ICO MIME type");}
"""
)
(OUT / "headers.c").write_text(code)
subprocess.run(
    [
        "cc",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        str(OUT / "headers.c"),
        "-o",
        str(OUT / "headers"),
    ],
    check=True,
)
subprocess.run([str(OUT / "headers")], check=True)
print("PASS active branding, recovery page, package identity, icon format")
