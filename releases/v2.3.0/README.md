# Wallbox Gateway v2.3.0

Significant release combining first community contributions with new
visibility features.

## 🙏 First community contributions — thanks @benvanmierloo!

Two excellent additions in [#1](https://github.com/botts7/esp32-wallbox/pull/1):

- **BLE SMP pairing for Wallbox firmware ≥ 6.11.26.** Newer charger firmware
  requires an encrypted BLE link before allowing notifications. The gateway
  now tries plain `registerForNotify` first (backwards-compatible with
  older firmware) and falls back to SMP passkey pairing using the
  configured BAPI PIN. **Unlocks compatibility for everyone on the newer
  firmware.** Tested on FW 6.11.16 (no fallback needed) so existing users
  see no change.
- **Telnet log server on port 23.** All `Serial` output is streamed to up
  to two LAN telnet clients, advertised via mDNS. Zero overhead when no
  client is connected. Connect with `telnet wallbox-gw.local` from any
  Linux/Mac/Windows machine on the LAN — finally, remote-debug your
  gateway without a USB cable.

## ☀️ Solar savings on the stats tiles

If you charge in Eco Smart mode (solar-aware), the gateway now shows
how much **money you saved** by using your own solar versus the grid
tariff. Visible as a yellow "saved $X" sub-line under the Week / Month
cost tiles on `/sessions`. Configurable green rate (default $0 = free
solar; set to your feed-in tariff to see opportunity cost).

## 🔔 Notifications: Home-Assistant route

Browser push notifications can't work over plain HTTP (browser security
restriction). The new **Notifications panel** (Settings → Charger → 🔔
Notifications) recognises this and instead gives you a ready-to-paste
**Home Assistant automation snippet** that delivers proper push
notifications via the HA Companion app, using our existing
`sensor.wallbox_pulsar_max_status` MQTT entity.

## ⚡ Faster page reveal

Page reveal now triggers on `DOMContentLoaded` instead of `window.load`.
On a busy gateway (e.g. weak BLE signal causing the firmware to fight
for radio time), pages now appear in well under a second rather than
waiting for every CSS/JS asset to fully load.

## Files

| File | SHA256 |
|------|--------|
| `wallbox-gateway-esp32s3-v2.3.0.bin` | `c7f1f0122fc2813f88b6a4a58308287cc2d234f29f1911ad44cccfe3a3524a40` |
| `bootloader.bin` | `1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343` |
| `partitions.bin` | `2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e` |

## Install

OTA from any v2.x: `http://wallbox-gw.local/ota` → upload the `.bin`.
Fresh install: see [v2.1.0 README](../v2.1.0/README.md#installation).

## Coming next

**v2.4 — Pulsar Plus compatibility** is in active prep. The transport
layer differs (separate RX/TX characteristics, OS-level pairing
required) but the BAPI protocol overlaps with Pulsar MAX. Looking for
Pulsar Plus owners willing to flash and share scan/telnet logs — open
an issue if interested.
