# Compatibility

What's confirmed working, what's in progress, and how to add your data point.

This list grows as users report. Reports of **either** working **or** broken setups are equally valuable — see [How to contribute a report](#how-to-contribute-a-report) at the bottom.

## Chargers

| Model | Firmware | Status | Notes |
|---|---|---|---|
| **Pulsar MAX** | 6.11.16 | ✅ Fully working | Reference device. All features tested. |
| **Pulsar MAX** | 6.11.26+ | ✅ Working (v2.3.0+) | Requires v2.3.0 (BLE SMP pairing). Older gateway versions will fail on notification subscription. |
| **Pulsar Plus** | _any_ | 🟡 In active prep | Branch [`v2.4-pulsar-plus`](https://github.com/botts7/esp32-wallbox/tree/v2.4-pulsar-plus) has dual-RX/TX-characteristic support staged. Looking for testers — see issue template `pulsar-plus-compat`. |
| **Copper SB** | _any_ | ⚪ Unknown | Protocol likely overlaps with Pulsar Plus. Reports welcome. |
| **Commander 2** | _any_ | ⚪ Unknown | Similar BAPI surface expected. Reports welcome. |
| **Quasar / Quasar 2** | _any_ | ⚪ Unknown | DC bidirectional models — `den` (discharge) field already in the protocol; UI is ready. Untested. |

Legend: ✅ working · 🟡 work in progress / partial · ⚪ unknown · ❌ blocked

## Gateway hardware

| Board | Status | Notes |
|---|---|---|
| **ESP32-S3-WROOM-1U-N16R8** (external IPEX antenna) | ✅ Recommended | Reference build. 16 MB flash, 8 MB PSRAM. The external antenna matters — internal-antenna boards have struggled at typical distances. |
| **ESP32-S3-DevKitC-1** | ✅ Should work | 8 MB flash version. Built-in PCB antenna will be marginal beyond ~5 m line of sight. |
| **ESP32-S3 with PSRAM** (generic) | ✅ Should work | Needs ≥ 8 MB flash for the OTA partition layout. |
| Plain **ESP32** (not S3) | ❌ Not supported | Build targets ESP32-S3 specifically. |

## Tested BLE signal strength

Empirical from the reference device:

| RSSI | Behaviour |
|---|---|
| ≥ −75 dBm | Healthy. Commands respond in ~1 second. Reliable reconnects. |
| −75 to −85 dBm | Usable. UI may feel slightly slow. Banner shows "Weak signal". |
| −85 to −95 dBm | Marginal. Commands often time out. Reconnect loops. Banner shows "Very weak". |
| < −95 dBm | Not reliably usable. Move the ESP32 closer or use a directional antenna. |

If you hit < −90 reliably, the **single highest-impact fix** is moving the ESP32 to within line-of-sight of the charger. The Wallbox BLE radio is weak compared to a typical ESP32-S3.

## Home Assistant

| HA version | Status | Notes |
|---|---|---|
| 2024.x and later | ✅ Working | MQTT discovery, Energy dashboard integration |
| Older than 2024.1 | ⚠ Likely works | Energy-dashboard wiring for the `meter_total_energy` sensor relies on `state_class: total_increasing` which is universally supported. Untested specifically. |

The gateway publishes the Wallbox device with native HA entities (sensors, switches, locks, numbers, selects). No HACS component needed — it's MQTT discovery.

## Protocols / firmware notes

- BAPI framing: `EaE` + length + JSON + checksum — appears stable across Wallbox models.
- BAPI method names (`r_dat`, `g_alo`, `w_cha`, etc.) — mostly shared across models. Some are Pulsar-Plus-only (grid code, multi-user, mobile connectivity).
- Authentication: Pulsar MAX uses BAPI-layer PIN (no OS pairing). Pulsar Plus appears to require OS-level BLE pairing before BAPI works (handled transparently in v2.3.0+).
- Newer firmware (≥ 6.11.26) on any model: requires encrypted BLE for notifications. v2.3.0+ handles this.

## How to contribute a report

1. Flash the latest release ([Releases page](https://github.com/botts7/esp32-wallbox/releases))
2. Try it on your charger
3. Open an issue using the appropriate template:
   - **Pulsar Plus owners** → [`pulsar-plus-compat`](https://github.com/botts7/esp32-wallbox/issues/new/choose)
   - **Anything else** → use the standard `bug_report` or `charger_compatibility` template
4. Include the **BLE Scan** result (Config page → BLE Scan button) and a snippet of telnet log if you have it
   - **Windows users:** the built-in `telnet` client isn't enabled by default (and you'd need to set it to send LF as CRLF). Easiest path is [PuTTY](https://www.putty.org/) in "Raw" mode pointing at port 23 — readable lines out of the box.
   - **macOS / Linux:** `nc wallbox-gw.local 23` works directly.

Even a one-line "works on my Pulsar MAX FW X.Y.Z, ESP32-S3 board Z, RSSI Q" is useful. Negative results also help — if it doesn't connect, the scan log tells us what to fix.

Discussions are open for general questions: <https://github.com/botts7/esp32-wallbox/discussions>
