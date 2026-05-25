# v2.4.0-rc5 — Pulsar Plus pre-release · live scan logs

⚠️ **Release candidate — testers only.** Pulsar MAX users on stable
v2.3.0 don't need to upgrade unless they want to help test.

Builds on [rc4](../v2.4.0-rc4/README.md) with one small UX fix.

## Fixed in rc5

- **BLE / WiFi scan progress streams to telnet in real time** ([#5](https://github.com/botts7/esp32-wallbox/issues/5)).
  Previously the `/api/ble-scan` and `/api/wifi-scan` endpoints ran
  silently and only wrote to the JSON response — meaning anyone
  watching a `telnet` session saw no output during an 8-second BLE
  scan. Now you get a clear "starting 8s scan..." line, followed by
  per-device listings, and a "complete: N device(s) seen" summary.

Reported by [@peter-mcc](https://github.com/peter-mcc).

## Everything else from rc4

Carries forward all rc4 fixes (OTA reliability via WDT extension +
Content-Length-sized erase, dynamic versioning, Pulsar Plus dropdown,
auto-BLE-pause on OTA, CRLF telnet output, etc.). Smoke-tested on
Pulsar MAX, no regression.

## How to flash

OTA from rc4 (or any v2.x): `http://wallbox-gw.local/ota` →
`wallbox-gateway-esp32s3-v2.4.0-rc5.bin`. After reboot, `/info`
footer should read `v2.4.0-rc5`.

## SHA256

```
0f343674c05d24297a71a63b03d38af5367373e262fadbedbe1ef3742e08d023  wallbox-gateway-esp32s3-v2.4.0-rc5.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```
