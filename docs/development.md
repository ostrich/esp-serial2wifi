# Development guide

- [Build](#build)
- [Test and develop the UI](#test-and-develop-the-ui)
- [Code and storage layout](#code-and-storage-layout)
- [Prepare a release](#prepare-a-release)
- [Source and dependencies](#source-and-dependencies)

## Build

Use x86-64 Linux with Python 3.12+, Node.js 22+, npm, GNU Make 4.3+, GCC,
binutils, tar and sed. Native tests use AddressSanitizer/UndefinedBehaviorSanitizer.
For other host architectures or operating systems, use an x86-64 Linux VM.

On Arch Linux:

```sh
sudo pacman -S --needed base-devel python nodejs npm
```

On Debian/Ubuntu, install `build-essential`, `binutils`, `python3`,
`python3-venv` and `ca-certificates`, plus Node.js and npm. Check that the installed
Python, Node and Make meet the versions above; older distribution releases may
need newer packages.

From the repository root:

```sh
make setup
make FLASH_SIZE=4MB
```

Setup creates `.venv/`, installs Python dependencies, runs `npm ci`, and downloads
checksum-pinned SDK/compiler archives into `.cache/esp8266/`. No preinstalled ESP
SDK, Java, Docker, PlatformIO or Arduino IDE is required. Interrupted setup can
be rerun. Do not bypass a checksum failure by changing the expected hash.

Use `FLASH_SIZE=512KB` for the smaller target. Build one configuration at a time
in each checkout; switching capacity rebuilds the affected files. Outputs are
`firmware/user1.bin`, `firmware/user2.bin` (4 MB), and
`firmware/release-manifest.json`. See the [user guide](user-guide.md#installation-and-updates)
for flashing; building and testing do not flash a device.

Copy `config.mk.example` to ignored `config.mk` for local wiring/build options,
or pass them on the command line:

```sh
make FLASH_SIZE=512KB VERSION=v0.1.1-dev1
```

Command-line options apply to that invocation only. In particular, pass the
capacity again when invoking a flash target. Set Wi-Fi credentials in the UI,
not in public builds. `VERSION` defaults to `v0.1.0` in `Makefile`; the manifest
records Git revision and source hashes separately.

Standard builds use DIO/40 MHz flash and embedded xterm 5.5. Debug traces can be
enabled with `SERIAL2WIFI_DEBUG=1`, which consumes additional space. The
`SERIAL2WIFI_EXTERNAL_UI=1` option uses banked UI storage and xterm 6 on 4 MB;
it is experimental and is not the normal release layout. Lower-level 1 MB/2 MB
build options exist but are outside the standard 512 KB/4 MB release matrix.

`make clean` removes `build/` and `firmware/`, keeping dependencies and `dist/`.
Ordinary builds use the prepared SDK offline. `make setup OFFLINE=1` also needs
an already installed Python environment, cached npm packages and local SDK
archives; it cannot prepare an empty checkout without downloads.

## Test and develop the UI

```sh
make check
make check-browser BROWSER=/path/to/chromium
make check-layout BROWSER=/path/to/chromium
```

`make check` builds firmware and runs native/model tests. Browser tests start
isolated headless profiles against synthetic fixtures; they do not attach to
personal tabs or a real ESP. Layout checks cover narrow/wide screens, scaling
and enlarged text. These checks do not establish hardware behavior.

To inspect the UI locally:

```sh
make ui
.venv/bin/python tests/fixture_server.py --port 8765 --assets build/web
```

Open the printed loopback URL. The fixture server rejects device writes. Use
[UI navigation notes](ui-navigation.md) for known routes and readiness checks.
Production assets are bundled/minified by `tools/build/build_ui.py`, then gzip
packed into ESPFS. The browser decompresses them; the ESP serves compressed bytes.
Content-addressed asset URLs prevent mixing modules across firmware updates.

### Tool organization and names

Normal builds use Make; the top-level tools are user commands:

- `tools/setup.py` prepares build dependencies (`make setup`).
- `tools/flash_serial.py` writes ESP firmware through a serial adapter.
- `tools/flash_ota.py` updates over Wi-Fi; `--check` validates without uploading.
- `tools/flash_mcu.sh -p optiboot|stk500v2` programs an attached AVR.

`tools/build/` contains build-only commands, `tools/lib/` contains shared flash
formats, and `tools/experimental/` holds the banked-UI installer. Release text
lives in `tools/templates/`. Commands use `verb_noun` names and language extensions;
Python modules use `snake_case`. `setup.py` is the single-word setup command.

The UI pipeline is one command, `tools/build/build_ui.py`, with `source`, `web`
and `firmware` stages for previews, optimized assets and ESPFS generation.
Keep format validation shared with the flash tools; avoid duplicating it in build
scripts or adding one-file wrappers around existing commands.

## Code and storage layout

| Location | Purpose |
| --- | --- |
| `src/core/` | Configuration, Wi-Fi, versioned API, flash and updates |
| `src/serial/` | UART, capture, TCP/Telnet, SLIP and SDK transport overrides |
| Other `src/` modules | HTTP server, ESPFS, MCU commands, MQTT, HTTP/socket clients, custom pages and syslog |
| `include/` | Shared platform headers |
| `web/app/` | Browser interface |
| `web/custom-pages/`, `web/artwork/` | MCU page helpers and icon source |
| `third_party/` | Vendored terminal distributions and notices |
| `tools/`, `mk/` | Setup, asset/image generation, packaging, flashing and build rules |
| `tests/` | Native/browser tests, stubs and synthetic fixtures |

The Non-OS SDK runs cooperative callbacks. Keep UART/TCP work bounded and retain
transmit buffers until copied or acknowledged. Build-time symbol rewriting
creates a shadow lwIP archive; it does not modify the downloaded SDK. Port-23
transport uses byte credit and measured retransmission timing to handle small
send windows and delayed acknowledgments. See `src/serial/sdk_rto.c`,
`src/serial/rto_policy.h` and their tests before changing these bindings. The RTT
policy has a 200 ms minimum and 60 s maximum; programming traffic is handled
separately.

Versioned API writes use POST and `X-Serial2WiFi-Request: 1`, with endpoint-specific
body validation. The header is a request guard, not authentication. HTTP and
serial access have no authentication or TLS. Configured passwords are redacted
from API reads. Custom pages and MCU callbacks are trusted application code.
Original upstream management endpoints and configuration migrations are not
supported; MCU protocol endpoints remain for the supported programming features.

| Standard build | Maximum application image | Update method |
| --- | ---: | --- |
| 512 KB | 479,232 bytes | Serial flashing |
| 4 MB | 503,808 bytes per slot | OTA or serial flashing |

Use `tools/lib/flash_layout.py` and the build manifest as the authority for addresses.
Configuration has two CRC-protected copies with a sequence commit protocol;
reserved bytes preserve field offsets. SDK settings, application configuration
and custom pages occupy separately guarded regions. OTA validates the inactive
slot before activation. The `ELSDK306-MAP…-CFG…` identities describe compatibility,
not branding. Hashes detect mismatches; they do not authenticate a publisher.

## Prepare a release

Use a clean checkout without `config.mk`, with default modules and wiring.
Run the native and browser/layout checks above, then build both capacities:

```sh
make release FLASH_SIZE=512KB VERSION=v0.1.0
make release FLASH_SIZE=4MB VERSION=v0.1.0
.venv/bin/python tests/test_release_archive.py dist/esp-serial2wifi-v0.1.0-512kb.tar.gz
.venv/bin/python tests/test_release_archive.py dist/esp-serial2wifi-v0.1.0-4mb.tar.gz
```

Each capacity archive has a matching `.sha256` file and contains its manifest, slot images,
boot/SDK initialization files, flashing tools and license notices. Its installation
README comes from `tools/templates/release_readme.md`. Never combine files across builds.
Combine the two standard capacity archives into a single download:

```sh
.venv/bin/python tools/build/package_release.py --combine \
  dist/esp-serial2wifi-v0.1.0-512kb.tar.gz \
  dist/esp-serial2wifi-v0.1.0-4mb.tar.gz --label v0.1.0
.venv/bin/python tests/test_release_archive.py dist/esp-serial2wifi-v0.1.0.tar.gz
```

The combined archive has self-contained `512kb/` and `4mb/` folders, plus a short
README explaining which to use. Combining rejects mismatched revisions, versions,
SDK inputs or checksums. Check manifest/version/tag agreement and report physical-device
testing separately.

Packaging fixes file order, permissions, owner IDs and timestamps
(`SOURCE_DATE_EPOCH`, default 0). Repacking unchanged inputs produces identical
archives; compare actual artifacts before claiming reproducibility across hosts.

### GitHub Actions

Pushes to `master`, pull requests to `master`, and manual Actions runs build both
capacities. Each build runs host tests and archive checks; the 4 MB job also runs
the browser and layout suites. Download firmware from the run’s **Artifacts**
section; those artifacts expire after 14 days. Successful master builds also
refresh the **Latest master build** prerelease on the repository’s Releases page.
Its download is `esp-serial2wifi-<8-character-commit>.tar.gz`, plus a SHA-256 file.
The `master-build` tag moves to that commit and old development downloads are
removed. Development firmware versions include the commit ID.

To publish a release, push a version tag on the intended commit:

```sh
git tag v0.1.0
git push origin v0.1.0
```

Use a new tag for each release. After both builds pass, the workflow creates a
GitHub release with `esp-serial2wifi-v0.1.0.tar.gz` (both capacities) and its
SHA-256 file. The tag
sets the firmware version; update the Makefile default when starting a new version.
Tags with a suffix, such as `v0.1.1-rc1`, produce prereleases. Failed builds do not
publish. Versioned releases are not overwritten; the master prerelease is replaced
after successful master builds. Stable releases remain marked as latest even
when a newer development prerelease is published.

The workflow uses pinned actions, Python 3.12 and Node.js 22 on x86-64 Ubuntu.
Only the publication job has repository write access. SDK archives and package
downloads are cached; firmware outputs and virtual environments are rebuilt.
CI never contacts or flashes an ESP; hardware testing remains separate.

## Source and dependencies

ESP Serial2WiFi derives from JeeLabs esp-link, esphttpd and the contributors in
[LICENSE](../LICENSE). The initial import came from maintained source revision
`f40af5fc8eaac709a754656a79f0e97385ba13c4`, including the Python 3 tooling, SDK3
transport work, live terminal and versioned APIs. Original author notices remain.

`sdk-lock.json` records archive URLs, revisions and SHA-256 hashes. The firmware
uses Espressif NONOS SDK 3.0.6, runtime `3.0.6-dev(072755c)`. Downloads remain
unchanged; manifests record build-time SDK bindings. Archives include the SDK
license alongside the project license.

Vendored xterm 5.5.0 and 6.0.0 under `third_party/` retain their licenses and
npm-source metadata/hashes. Other upstream notices, including Pure CSS, remain
in LICENSE and assets. Node dependencies are pinned in `package-lock.json`;
Python dependencies are in `requirements-build.txt` and `requirements-flash.txt`.
