# User guide

- [Installation and updates](#installation-and-updates)
- [Wiring and first connection](#wiring-and-first-connection)
- [Network settings](#network-settings)
- [Using the serial port](#using-the-serial-port)
- [Optional MCU features](#optional-mcu-features)
- [Troubleshooting](#troubleshooting)

## Installation and updates

Use firmware for your board’s actual flash capacity. Keep images together with
their `release-manifest.json`; it records their slot, addresses and hashes.
Do not mix images from different builds. Original esp-link settings are not
migrated: use a factory installation when moving from that firmware or an unknown
flash layout.

**Before any operation that resets the ESP**, disconnect its TX from the attached
device’s RX, or put the attached device in a safe state. ESP ROM boot messages
can be interpreted as input, particularly by a logged-in shell. Debug UART Off
does not suppress those messages. Keep power connected throughout flashing.

### Back up first

From a source checkout, run `make setup` as described in the
[development guide](development.md). Select your USB-to-serial port explicitly:

```sh
mkdir -p backups
.venv/bin/python -m esptool --chip esp8266 --port /dev/ttyUSB0 read-flash 0 ALL backups/before.bin
sha256sum backups/before.bin
```

Confirm the backup size matches the flash capacity. Keep it private: it can
contain passwords. `backups/` is ignored by Git.

A board with a USB UART and automatic reset circuit usually enters flashing mode
itself. With a bare module, provide stable power and a 3.3 V USB UART, connect
TX/RX and ground, and enter its UART download mode using the board’s boot/reset
controls. Release the boot control before restarting into the application.

### Factory installation

This **erases all flash**, including settings and custom pages, then writes the
bootloader, application and SDK initialization data:

```sh
make factory-flash FLASH_SIZE=4MB PORT=/dev/ttyUSB0
```

For a 512 KB board, use `FLASH_SIZE=512KB` in that command. The flash targets
build before writing, so specify the capacity on the flashing command itself,
or save it in `config.mk`. A preceding build does not change the default.

If using an extracted release archive instead of source, follow its included
README; its `flash_serial.py` uses the archive’s manifest and needs no compiler.

### Updating over Wi-Fi

On a supported 4 MB device, open **Firmware**, select the image for the target
slot shown on screen, upload it, then activate it. Activation restarts the ESP.
Reconnect and reload the page to check the version and active slot.

From a source checkout, the equivalent command is:

```sh
.venv/bin/python tools/flash_ota.py <device-address> firmware/user1.bin firmware/user2.bin
```

The tool checks the SDK/layout and slot, uploads once, activates once and checks
the running slot. There is no automatic boot rollback. If a reply is lost, inspect
the running firmware before retrying. Changing SDK/layout requires serial flashing.
512 KB devices cannot use OTA because they have no inactive application slot.

### Updating through USB-to-serial

For an application-only update with a matching layout:

```sh
make flash FLASH_SIZE=4MB PORT=/dev/ttyUSB0 SERIAL_SLOT=1
```

Set the actual capacity and active slot. This preserves settings but **does not
change which slot boots**. A 512 KB device has only slot 1. Prefer OTA for normal
4 MB updates.

## Wiring and first connection

Use 3.3 V TTL signals, not RS-232 voltages or 5 V logic. Connect ESP TX to the
attached device’s RX, ESP RX to its TX, and share ground. Use a suitable power
supply for your ESP board; a USB UART’s power output may not be sufficient.

After factory installation, join **ESP-Serial2WiFi-xxxxxx** and visit
**http://192.168.4.1/**. The suffix identifies the ESP chip. Set an AP password
under **Network** before using the attached device. There is no separate web or
serial login on the ESP and no TLS; use a protected AP or trusted network.

Fresh defaults are 115200 baud, 8 data bits, no parity, 1 stop bit, with TX on
GPIO1 and RX on GPIO3. Optional reset/ISP/LED pins, debug UART, time service and
MCU integrations start disabled. Existing supported settings survive ordinary
application updates.

Use **Serial & MCU** to match the attached device’s baud and framing. Under
**Device settings → GPIO and UART routing**, configure any auxiliary pins.
Board presets fill the form; they are not board detection, and apply only when
saved. Check the wiring before saving. Swapped UART uses TX GPIO15 and RX GPIO13;
GPIO0, GPIO2 and GPIO15 also affect how the ESP boots. MCU reset/ISP settings
control the attached device, not the ESP itself.

## Network settings

Choose **Standalone Wi-Fi** to connect directly, or **Join a network** to use a
router’s 2.4 GHz Wi-Fi. Selecting a tab or scanning does not change the connection;
save the form to apply it.

Standalone Wi-Fi has its own name, password, channel and IPv4 address, with a
/24 subnet and DHCP for clients. Its default address is `192.168.4.1`. Router
connections use DHCP unless you select a static address in that section.

**Hostname & discovery** sets the hostname and mDNS options. Restart to apply
those changes. With mDNS enabled and supported by your computer, use
`<hostname>.local`. Changing the hostname does not rename the Wi-Fi network or
change its address. Saved passwords are never displayed in the forms.

Saving network changes can disconnect you. Follow the page’s connection status,
then reconnect using the new network/address. If joining a router fails, use the
recovery AP to correct the settings. If the save reply was lost, check the saved
configuration before submitting again.

## Using the serial port

Open **Serial & MCU** and click the terminal to type. It restores the small amount
of recent output still buffered on the ESP, then receives live output. This is
not a full session recording; a quiet device may show a blank screen. Opening
the page does not send a newline or command to request a prompt.

The live terminal waits if another interactive client owns the port. Close or
pause the other browser terminal, or close its TCP session, to switch clients.
**Pause terminal** stops this browser’s live session; it does not stop the attached
device. Raw serial tools pause terminal emulation and its automatic replies, and
let you send explicit text or bytes to devices that do not use terminal controls.

**Terminal options** changes the browser’s rows and columns. Devices that query
terminal dimensions receive that size; other devices must be configured separately.
The optional **Unix shell** controls copy or send an `stty` command. Use them only
at an empty Unix shell prompt; they are not needed for other serial devices.

For GNU Telnet:

```sh
telnet -8 <device-address>
```

`-8` requests eight-bit operation. Omit the explicit port so GNU Telnet starts
Telnet negotiation; port 23 is the default. Port 23 also supports raw TCP clients.
Port 2323 is for MCU programming and can pulse configured reset/ISP pins.

## Optional MCU features

The serial bridge works without these features.

**Integrations** lets compatible MCU firmware use the ESP for MQTT, outbound
HTTP and socket connections. It requires ELClient or an implementation of the
esp-link command protocol carried over SLIP. SLIP is a standard framing method;
the commands inside it are specific to that protocol. Enabling it does not make
an ordinary serial device understand networking commands. For MQTT, configure
the broker and check the reported connection state after saving. These connections
are unencrypted. Time service and Syslog are under **Device settings**; the time
service uses a fixed whole-hour UTC offset and does not adjust for DST.

**Custom web pages** hosts small displays or controls at `/user/<filename>`.
Upload HTML fragments without doctype/html/head/body tags: the ESP supplies the
document wrapper, styles and helper scripts. Include supporting JavaScript/CSS
files as needed. Interactive MCU callbacks require compatible MCU firmware and
Microcontroller integration. Uploading replaces the entire file set, so include
all files you want to keep and retain copies locally.

**Serial & MCU → MCU programming and reset** updates the attached MCU, not the
ESP. The browser programmer supports ATmega328P with Optiboot and a 512-byte
bootloader area. Set the reset pin and bootloader baud rate, close other serial
clients, select an Intel HEX file and follow the validation/programming steps.
It verifies flash readback. The external-programmer section gives AVRDUDE examples
for ATmega328P and ATmega2560; check the bootloader and wiring before using them.
The source also includes `tools/flash_mcu.sh`: select `-p optiboot` for an Uno
or `-p stk500v2` for a Mega. Use **Firmware**
to update the ESP itself.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Garbled serial output | Match baud, data bits, parity and stop bits; check crossed TX/RX, ground and signal levels. |
| Blank terminal | The device may be quiet. Check wiring/settings; send input only if appropriate for that device. |
| “Serial port in use elsewhere” | Check other browser windows and TCP clients. Pause or close the other session. |
| Missing output | Diagnostics separates UART, TCP and web-terminal losses. Compare counters to see whether losses are new. |
| Cannot connect after a change | Check the selected Wi-Fi network and new address; use the recovery AP after a failed router connection. |
| Update reply lost | Reconnect and check version/slot before repeating an upload or activation. |

Diagnostics counters reset when the ESP restarts. Recorded errors alone do not
mean data is still being lost. A failed browser reconnect likewise does not prove
that a firmware update failed.
