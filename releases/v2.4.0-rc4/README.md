# v2.4.0-rc4 — Pulsar Plus pre-release · OTA reliability · dynamic version

⚠️ **Release candidate — testers only.** Pulsar MAX users on a stable
v2.3.0 don't need to upgrade unless they want to help test.

Builds on [rc3](../v2.4.0-rc3/README.md) with the actual OTA fix and a
dynamic version display.

## Fixed in rc4

### OTA upload reliability ([#4](https://github.com/botts7/esp32-wallbox/issues/4))

Root cause was subtler than the BLE-stress theory from rc3:

- `Update.begin(UPDATE_SIZE_UNKNOWN)` erases the **entire 1.9 MB OTA
  partition** before accepting any bytes. That blocks the HTTP server
  for 10+ seconds.
- During that block, the **ESP32 Task Watchdog** (default 5 s) fires
  and **panic-reboots** the device mid-upload.
- Symptoms: Chrome stalled at 8 %, Firefox at 43 % (same crash, different
  client-side buffering). Uptime counter went back to 0 after each
  attempt.

Fix:

- `esp_task_wdt_init(60, false)` at OTA start to give the erase room to
  complete.
- `Update.begin(size)` now uses the actual `Content-Length` from the
  upload header so the erase is sized to the firmware, not the whole
  partition.
- Combined effect: OTA on this gateway dropped from ~16 s to **~8 s**.

The BLE-pause-on-OTA from rc3 is kept as a belt-and-braces measure.

### Version display ([#3](https://github.com/botts7/esp32-wallbox/issues/3))

- `/info` page footer, boot log, and mDNS service TXT record now all
  use a `WB_VERSION` macro instead of hardcoded `"v1.0"` / `"v2.0"`.
- `WB_VERSION` is **injected at build time** by `scripts/version.py`
  using `git describe --tags --dirty`. Single source of truth, no
  manual bumps. Untagged dev builds get `v2.4.0-rc4-N-gabc1234-dirty`
  so you always know which code is running.

## Everything else from rc3 / rc2 / rc1

Pulsar Plus dropdown + Nordic-UART preset, dual-char BLE mode, CRLF
telnet line endings, auto-BLE-pause on OTA. MAX path smoke-tested,
no regression.

## How to flash

Browser OTA should now work even with a flaky BLE state:

1. `http://wallbox-gw.local/ota` → upload
   `wallbox-gateway-esp32s3-v2.4.0-rc4.bin`
2. After reboot → `/config` → expand **Advanced**
3. **Charger model** → `Pulsar Plus` (or leave at MAX) → Save & reboot

Verify on `/info` — footer should now read `v2.4.0-rc4`.

## SHA256

```
ceb16b95c6ff7f950307642b75d084adc2b16fcbc088e65af8bd3ef762aea2c9  wallbox-gateway-esp32s3-v2.4.0-rc4.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```
