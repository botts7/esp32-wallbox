# v2.4.0-rc1 — Pulsar Plus pre-release

⚠️ **Release candidate — for compatibility testers only.** Pulsar MAX
users on a working v2.3.0 setup should **not** upgrade to rc1 unless
they want to help test.

## What this is for

Adds **Pulsar Plus** support (dual RX/TX BLE characteristics + charger
model presets) on top of v2.3.0. **Untested on Plus hardware** — that's
the whole point of this rc.

If you're a Pulsar Plus owner: flash this, pick **Pulsar Plus** in the
new "Charger model" dropdown under Config → Advanced, save, and tell us
what happens. Pasting the **telnet log** during connection attempt is
the single most useful thing you can do.

## What's new vs v2.3.0

- **Charger model dropdown** in Config → Advanced. Choices: *Pulsar MAX*
  (default — u-blox single-char), *Pulsar Plus* (Nordic UART dual-char),
  *Custom* (manual UUIDs).
- **Dual-characteristic BLE mode** — write to one characteristic, listen
  for notifications on a different one. Required for Pulsar Plus.
- **Pulsar Plus preset UUIDs** auto-fill on save when you pick that
  model (from
  [jagheterfredrik/wallbox-ble](https://github.com/jagheterfredrik/wallbox-ble)).
- Carry-overs from main since v2.3.0: auth-nudge banner, weekly schedule
  timeline view, `COMPATIBILITY.md` doc, GitHub Discussions link.

## Pulsar MAX users — what changes for you?

**Nothing functional.** The default charger model is still MAX, the
default UUIDs are still u-blox, single-char mode is still the only path
exercised for MAX. The Plus code is purely additive.

This rc was smoke-tested on a Pulsar MAX before publishing.

## How to test (Pulsar Plus owners)

1. **OTA from v2.3.0**: `http://wallbox-gw.local/ota` → upload
   `wallbox-gateway-esp32s3-v2.4.0-rc1.bin`. Or fresh-flash via ESP Web
   Tools / esptool — see [v2.1.0 README](../v2.1.0/README.md#installation).
2. After reboot, go to `http://wallbox-gw.local/config` → expand the
   **Advanced** section → set **Charger model** to `Pulsar Plus` →
   Save. The UUIDs auto-fill.
3. Set your charger's BLE MAC in the BLE Address field if not already.
4. Save and reboot.
5. Open a second terminal: `telnet wallbox-gw.local` (Win10+ has a
   client built in; macOS/Linux: `nc wallbox-gw.local 23`).
6. Watch the log during the connection attempt.
7. **Report what you see** — issue template
   [`pulsar-plus-compat`](https://github.com/botts7/esp32-wallbox/issues/new/choose),
   or just reply on the
   [HA forum thread](https://community.home-assistant.io/t/wallbox-gateway-local-ble-mqtt-for-pulsar-max-no-cloud-full-ha-control/1007464).

## SHA256

```
93a8859ee2c11f8ec08958dff5f3f8d70e167db44d83f0d5a54ab175d908a366  wallbox-gateway-esp32s3-v2.4.0-rc1.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```

## Rolling back

If anything goes wrong, the gateway's smart-rollback partition means it
will revert to v2.3.0 after 3 failed boot attempts. Or just OTA the
v2.3.0 binary back in:
[releases/tag/v2.3.0](https://github.com/botts7/esp32-wallbox/releases/tag/v2.3.0).
