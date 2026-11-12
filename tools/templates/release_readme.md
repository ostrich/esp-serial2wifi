# ESP Serial2WiFi firmware

Use the archive for your board’s flash capacity. Keep its files together;
`release-manifest.json` records their addresses, hashes and compatible layout.

Before resetting or flashing the ESP, disconnect its TX from the attached
device’s RX, or put that device in a safe state. ROM boot bytes can be interpreted
as input. Keep power connected throughout the update.

## Update over Wi-Fi (4 MB)

For a device already running a compatible SDK/layout:

```sh
python3 flash_ota.py <device-address> user1.bin user2.bin
```

This tool uses only the Python standard library. It checks compatibility, uploads
to the inactive slot and activates it. Reconnect and check the running version.
There is no automatic boot rollback. If a reply is lost, check the version/slot
before retrying. Reload open web-interface tabs after updating.

## Install through USB-to-serial

Use Python 3.12+ and explicitly select the intended serial port:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-flash.txt
.venv/bin/python -m esptool --chip esp8266 --port /dev/ttyUSB0 read-flash 0 ALL before.bin
```

Check that the backup size matches the board’s flash capacity. Keep it private;
it may contain passwords. Then install:

```sh
.venv/bin/python flash_serial.py --factory --port /dev/ttyUSB0
```

**Factory installation erases all flash**, including settings and custom pages.
Use it for original esp-link firmware or an unknown layout; upstream settings
are not migrated. The tool verifies image and initialization-file hashes before
erasing. Boards without automatic reset need their boot/reset controls to enter
UART download mode.

For an application-only update with a matching layout, omit `--factory` and
select the active slot with `--slot 1` or `--slot 2`. Settings are preserved, but
the operation does not change which slot boots. A 512 KB device has only slot 1
and must use serial flashing because it has no OTA slot.

## First connection

Join **ESP-Serial2WiFi-xxxxxx**, open **http://192.168.4.1/** and set a Wi-Fi
password under Network. The initial AP is open. Default UART is 115200/8N1,
TX GPIO1 and RX GPIO3; use 3.3 V TTL and a shared ground. Auxiliary GPIO functions,
debug UART and optional services start disabled. Open Serial & MCU for the live
terminal. Ordinary updates preserve supported existing settings.

This project derives from JeeLabs esp-link and esphttpd. License notices are in
`LICENSE` and `licenses/`. Building this archive does not itself establish that
it has been tested on hardware.
