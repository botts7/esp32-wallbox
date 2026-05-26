# Installation

Three ways to flash the gateway, in increasing order of effort.

## 🪄 Easiest: one-click ESP Web Tools button

The repo's main README has an **Install** button. Plug the ESP32-S3 in via
USB, click the button, pick the COM port, hit Install. The browser
handles erase + flash of all three files at the correct offsets.

Requires a recent **Chrome** or **Edge** (Web Serial API). Doesn't work
on Safari or Firefox.

The button always installs the **latest stable release** (currently
v2.3.0). For pre-releases, see *Specific version* below.

## 🌐 Browser-based with file-picker — [esptool.spacehuhn.com](https://esptool.spacehuhn.com)

Useful if the one-click button isn't an option (e.g. testing a pre-release).

1. Download all three files from the
   [release page](https://github.com/botts7/esp32-wallbox/releases) you
   want to flash:
   - `bootloader.bin`
   - `partitions.bin`
   - `wallbox-gateway-esp32s3-vX.Y.Z.bin`
2. Open <https://esptool.spacehuhn.com>
3. **Connect**, then **Erase** (only required on first install or to
   recover a stuck board)
4. Upload all three files **at these offsets** — order doesn't matter,
   but the offsets do:

   | File | Offset |
   |---|---|
   | `bootloader.bin` | `0x0` |
   | `partitions.bin` | `0x8000` |
   | `wallbox-gateway-esp32s3-vX.Y.Z.bin` | `0x10000` |

5. Click **Program**, wait for completion, then **Reset**.

> ⚠️ If you only flash the firmware (`0x10000`) without updating
> bootloader and partition table on a fresh board, the board can end
> up in a half-broken state where the bootloader runs but the app
> doesn't boot. Always flash all three on a first install.

## 🧑‍💻 Command-line — `esptool.py`

For automation or comfort with the terminal:

```bash
pip install esptool

# First time / recovery: full erase
esptool.py --chip esp32s3 --port COM4 erase_flash

# Flash all three files
esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash \
  0x0     bootloader.bin \
  0x8000  partitions.bin \
  0x10000 wallbox-gateway-esp32s3-vX.Y.Z.bin
```

Replace `COM4` with your serial port (`/dev/ttyUSB0` or `/dev/cu.usbmodemXXX`
on macOS/Linux).

## 🔄 Subsequent updates: OTA

Once the gateway is on your LAN, all future updates can be done
in-browser:

1. Open `http://wallbox-gw.local/ota`
2. Upload the new `wallbox-gateway-esp32s3-vX.Y.Z.bin` (just the
   firmware, no bootloader / partitions needed)
3. Wait for "Update complete" — the gateway reboots into the new
   version

If OTA fails, falling back to USB always works.

## 🚨 Recovering a board that won't boot

Symptoms: USB connects, you see the ROM bootloader output
(`ESP-ROM:esp32s3-...`, `entry 0x...`) but then nothing — no Wallbox
boot banner.

This usually means the app partition got corrupted (often by a
mismatched bootloader/partition table). To recover:

```bash
# Full erase wipes everything to 0xFF
esptool.py --chip esp32s3 --port COM4 erase_flash

# Then flash all three files at their offsets (above)
```

Or in esptool.spacehuhn.com: **Erase**, then upload all three files at
their offsets.

If a full-erase + 3-file flash still doesn't bring the board back, the
flash chip itself is probably damaged from repeated failed OTAs or
just an unlucky cell. Time for a new board.

## Specific version

The latest-release URL pattern is:

```
https://github.com/botts7/esp32-wallbox/releases/download/<TAG>/<FILE>
```

For example, to grab the v2.4.0-rc5 firmware:

```
https://github.com/botts7/esp32-wallbox/releases/download/v2.4.0-rc5/wallbox-gateway-esp32s3-v2.4.0-rc5.bin
```

Each release page also has an `install.json` you can point an ESP Web
Tools button at to install that specific version one-click.

## Hardware

See [COMPATIBILITY.md](COMPATIBILITY.md) for the recommended ESP32-S3
boards and BLE-signal expectations. **ESP32-S3-WROOM-1U-N16R8 with
external IPEX antenna** is the reference build; cheaper PCB-antenna
variants work but range is marginal beyond a few metres.
