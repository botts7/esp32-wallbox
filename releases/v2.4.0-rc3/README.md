# v2.4.0-rc3 — Pulsar Plus pre-release (BLE-pause-on-OTA fix)

⚠️ **Release candidate — for compatibility testers only.** Pulsar MAX
users on stable v2.3.0 don't need to upgrade unless they want to help test.

Builds on [v2.4.0-rc2](../v2.4.0-rc2/README.md) with one further fix
from real-world feedback.

## Fixed in rc3

- **OTA upload no longer fails under BLE stress.** When a gateway is in
  a tight BLE reconnect loop (e.g. wrong UUIDs for the charger model),
  every scan/connect cycle drops the radio coex preference to BT and
  starves WiFi for several seconds at a time — fatal to a streaming OTA
  TCP upload. Symptom was an "upload error" at ~8% on a flaky-BLE
  gateway. The OTA handler now pauses BLE for 5 minutes at upload start,
  guaranteeing a clean WiFi window for the entire flash.
  - Reported by [@peter-mcc](https://github.com/peter-mcc) on the HA
    forum thread
    [#16](https://community.home-assistant.io/t/wallbox-gateway-local-ble-mqtt-for-pulsar-max-no-cloud-full-ha-control/1007464/16).

## Everything else from rc2

Carries forward [rc2](../v2.4.0-rc2/README.md)'s telnet CRLF fix and
[rc1](../v2.4.0-rc1/README.md)'s Pulsar Plus dropdown + dual-char BLE
mode. Pulsar MAX behaviour smoke-tested, no regression.

## Recommended path if your gateway is currently failing OTA

USB-flash this rc3 once via esptool, then future OTAs will Just Work
even if BLE is misbehaving.

```bash
esptool.py --chip esp32s3 --port COMx --baud 921600 write_flash \
  0x0     bootloader.bin \
  0x8000  partitions.bin \
  0x10000 wallbox-gateway-esp32s3-v2.4.0-rc3.bin
```

Or use [ESP Web Tools](https://esp.huhn.me/) in Chrome/Edge.

After it boots: `/config` → expand **Advanced** → **Charger model** =
`Pulsar Plus` → Save. Reboot. Done.

## SHA256

```
f1f6a81c25ce027d07842a1a6a8bd9cb3894c76d973685ac7acffc6502b74803  wallbox-gateway-esp32s3-v2.4.0-rc3.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```
