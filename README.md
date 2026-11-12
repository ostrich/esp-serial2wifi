# ESP Serial2WiFi

Connect a TTL serial device to Wi-Fi with an ESP8266. Use the browser terminal
or Telnet/TCP, configure the adapter through its web interface, and update 4 MB
devices over Wi-Fi. The attached device can be a microcontroller, a serial
console, or any other compatible device; Linux is not required.

- Live terminal with recent-output replay and raw text/hex tools.
- Configurable UART, GPIO routing, standalone Wi-Fi and router connection.
- Optional MCU programming, MQTT/HTTP/socket integration and custom web pages.
- Local web assets: no internet connection needed to use the adapter.

The standard 512 KB and 4 MB builds have the same application features, except
512 KB devices require serial flashing instead of OTA updates.

## Build

On **x86-64 Linux**, install Python **3.12+**, Node.js **22+**, npm, GNU Make
**4.3+**, GCC, binutils, tar and sed. Then run:

```sh
make setup
make FLASH_SIZE=4MB
```

The firmware identifies the release tag or Git commit it was built from;
uncommitted source changes add `-dirty`. Use `VERSION=...` to override it.

Use `FLASH_SIZE=512KB` for a 512 KB board. Setup installs dependencies inside the
checkout and downloads the pinned SDK/compiler. Images appear in `firmware/`.
See the [development guide](docs/development.md) for host setup, tests and releases.
Download firmware from [Releases](https://github.com/ostrich/esp-serial2wifi/releases).
Each `.tar.gz` contains separate `512kb/` and `4mb/` folders with installation
instructions. **Latest master build** is the rolling development prerelease;
versioned releases are the stable downloads. Both are built automatically.

## Install and use

Follow the [installation instructions](docs/user-guide.md#installation-and-updates)
to back up and flash the ESP through a USB-to-serial connection. Factory flashing
erases settings and custom pages; use the build for the board’s actual capacity.

A fresh installation creates an open **ESP-Serial2WiFi-xxxxxx** network. Join it,
open **http://192.168.4.1/** and set a Wi-Fi password under **Network**. Then wire
the serial device and open **Serial & MCU**. Defaults are **115200 baud, 8N1**,
TX on GPIO1 and RX on GPIO3. Use **3.3 V TTL**, cross TX/RX and share ground.

For GNU Telnet, run `telnet -8 <device-address>` without an explicit port.
Port 23 also accepts raw TCP; port 2323 is for programming an attached MCU.

ESP resets send boot bytes over UART, even with debug logging off. Before an
update, disconnect ESP TX from the attached device or put that device in a safe
state. See the [user guide](docs/user-guide.md) for wiring, networking, terminal
behavior and optional features.

## Contribute

See [CONTRIBUTING.md](CONTRIBUTING.md) and the
[development guide](docs/development.md). This project derives from JeeLabs
esp-link and esphttpd; original notices are preserved in [LICENSE](LICENSE).
Dependency and licensing details are in the
[development guide](docs/development.md#source-and-dependencies).
