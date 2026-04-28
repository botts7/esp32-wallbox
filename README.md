# ESP32 Wallbox BLE Gateway

> **Local BLE → MQTT gateway for the Wallbox Pulsar MAX. Full Home Assistant control with zero cloud, in ~$15 of hardware.**

<p align="center">
  <img src="docs/screenshots/dashboard.png" alt="Wallbox Gateway dashboard" width="320">
</p>

[![Latest release](https://img.shields.io/github/v/release/botts7/esp32-wallbox?color=4fc3f7)](https://github.com/botts7/esp32-wallbox/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Stars](https://img.shields.io/github/stars/botts7/esp32-wallbox?style=flat&color=yellow)](https://github.com/botts7/esp32-wallbox/stargazers)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange)](https://platformio.org/)
[![Home Assistant](https://img.shields.io/badge/Home%20Assistant-MQTT%20Discovery-blue)](https://www.home-assistant.io/)

✓ **No cloud** · talks BLE directly to your charger
✓ **HA Energy dashboard** ready — lifetime kWh sensor wired
✓ **~30 entities** auto-discovered (start/stop, schedules, eco-smart, halo, current limit…)
✓ **Self-hosted web UI** — dashboard, weekly heatmap, daily charging totals, CSV export
✓ **OTA updates** with rollback, captive-portal first-boot

> **Disclaimer:** Independent open-source project. Not affiliated with, endorsed by, or connected to Wallbox Chargers SL. Use at your own risk; modifying charger settings may void your warranty.

## More screenshots

| Sessions / Heatmap | Home Assistant |
|---|---|
| ![Settings](docs/screenshots/settings.png) | ![HA Device](docs/screenshots/ha-device.png) |

## Why this exists

The Wallbox app requires cloud connectivity for everything — even pressing "start charging" goes
through their servers, adds latency, and breaks when the internet is down. This gateway talks
**directly to your charger over Bluetooth**, giving you:

- **Instant response** (no cloud round-trip)
- **Works offline** (internet down? gateway still works)
- **Private** (nothing leaves your local network)
- **Scriptable** (full MQTT API for automations)

## Architecture

```
Home Assistant  ◄──MQTT──►  ESP32 Gateway  ◄──BLE (BAPI)──►  Wallbox Charger
```

The ESP32 sits within Bluetooth range (~10m) of your charger, maintains a persistent BLE
connection, and bridges BLE commands to MQTT with full Home Assistant auto-discovery.

## Features

### Live monitoring
- Charging power (kW), current per phase (A), session energy (kWh)
- Total lifetime energy from the charger's MID meter
- Mains voltage, whole-house power consumption
- Charger status (Ready / Plugged In / Charging / Scheduled / etc.)
- BLE signal strength

### Control
- Start / stop / pause / resume charging
- Lock / unlock charger socket
- Adjust max charging current (6-32A slider)
- Reboot charger

### Settings
- Charge schedules with day/time/power/energy limits (full UI editor)
- Auto-lock configuration (enable + timeout)
- Eco Smart solar charging modes (Off / Solar+Grid / Full Green)
- Power sharing, phase switching
- Halo LED brightness
- OCPP configuration (URL, charger ID, password)
- Timezone selection
- Weekly sessions heatmap

### Home Assistant integration
- 30+ auto-discovered entities (sensors, switches, numbers, selects, buttons)
- Proper native HA types (Eco Smart = select dropdown, Auto Lock = switch, etc.)
- Energy Dashboard compatible
- Time-of-use cost tracking examples in [HA docs](docs/HOME_ASSISTANT.md)

### Web UI
- Dark-themed responsive dashboard
- 4-page navigation: Dashboard, Settings, Config, Info
- PWA installable ("Add to Home Screen" on mobile)
- Toast notifications, inline editors, loading spinners

### Admin
- Captive portal AP mode for first-boot setup
- Web config UI with NVS persistence
- OTA updates (web upload + ArduinoOTA)
- Dual partition table with automatic rollback on failed updates
- mDNS: `http://wallbox-gw.local`
- Optional web authentication (rate limiting + lockout)
- CSRF protection on state-changing endpoints

## Compatible Chargers

Tested: **Wallbox Pulsar MAX** (u-blox NINA-B22 BLE radio, firmware 6.11.16)

Should work (same BAPI BLE protocol family):
- Pulsar Plus
- Commander 2
- Copper (SB / Business)
- Quasar / Quasar 2 (V2H discharge entity will populate on these)

The BLE radio varies by model — u-blox, Zentri (Silicon Labs), BGX. Service/Characteristic
UUIDs can be overridden in Config → Advanced.

## Hardware

- **ESP32-S3** dev board (recommended) or any ESP32 with BLE 4.2+
  - [ESP32-S3-WROOM-1U-N16R8](https://www.espressif.com/en/products/modules/esp32-s3) with IPEX antenna = best range
  - ESP32-S3-DevKitC-1 = easiest (built-in antenna)
- USB-C cable for initial flashing (OTA thereafter)
- 5V USB power supply (any phone charger works)

No wiring, sensors, or peripherals needed — just the ESP32.

## Quick Start

### Option 1: Pre-built Binary (easiest)

1. Download `firmware.bin` from [Releases](../../releases)
2. Flash using [ESP Web Tools](https://esp.huhn.me/) (in-browser) or esptool:
   ```bash
   esptool.py --port COM4 write_flash 0x0 firmware.bin
   ```
3. Connect to WiFi AP `WallboxGW-Setup` (password `wallbox123`)
4. Open `http://192.168.4.1/` in a browser
5. Configure WiFi, MQTT, BLE address (tap Scan to find your charger)
6. Save & Reboot

### Option 2: Build from source

1. Install [PlatformIO](https://platformio.org/install) (VSCode extension or CLI)
2. Clone this repo
3. Open in VS Code with PlatformIO
4. `pio run -e esp32s3 -t upload` (or click the → arrow)
5. Follow captive portal setup (step 3 above)

### After first-boot setup

- Dashboard: `http://wallbox-gw.local/` (or the IP shown in Config page)
- Future updates via OTA: Config → Firmware Update → upload new `firmware.bin`
- HA entities appear automatically under device "Wallbox Pulsar MAX"

## Home Assistant Setup

See [docs/HOME_ASSISTANT.md](docs/HOME_ASSISTANT.md) for:
- Complete entity list
- Dashboard YAML examples
- Automation recipes (solar surplus charging, charge complete alerts, off-peak scheduling)
- Time-of-use cost tracking with `utility_meter` (2-tier, 4-tier, seasonal)
- Energy Dashboard configuration

## Security

### BLE Security

Most Wallbox chargers ship with **no BLE PIN set** — anyone within ~10m of your charger can
control it. The gateway will warn you about this on the dashboard.

**To secure BLE**:
1. Open the Wallbox app, Settings → Security, set a PIN
2. In gateway Config → BLE → enter the same PIN
3. Gateway will auto-authenticate on each connection

### Web UI Security

Optional username/password authentication (Config → Web Security):
- Rate limiting: 1s delay per failed login
- 30s lockout after 5 failures
- CSRF tokens on all state-changing endpoints
- HTTPS not supported (ESP32 limitation — keep on trusted LAN)

### OTA Security

- ArduinoOTA password protected — defaults to `wb-XXXXXX` (last 6 hex of MAC, shown in serial log on boot). If a web auth password is set, that's used instead.
- Web OTA validates firmware magic byte before writing
- Smart rollback: firmware only validated after WiFi reconnects successfully
- BLE paused during OTA for reliable upload

## Troubleshooting

### BLE not connecting

**"Charger not visible — out of range or asleep"**
- Charger may be in sleep mode. Touch the keypad or plug in a car to wake it.
- RSSI weaker than -80 dBm = unreliable. Move ESP32 closer (even 2m makes a huge difference).
- If your ESP32 has an IPEX connector, ensure the external antenna is firmly seated.

**"Connection failed (RSSI -XX)"**
- Signal is borderline. Move closer or use external antenna.
- PC Bluetooth may be holding a connection. Disable BT on your PC to test.

### HA entities show "Unavailable"

- Gateway marks entities offline after 60s of BLE disconnect
- Check `http://wallbox-gw.local/` — if BLE bar is red, charger is out of range
- Restart HA MQTT integration if entities don't reappear within 2 minutes

### Can't reach `wallbox-gw.local`

- mDNS doesn't work on all devices (especially Android Chrome without DNS-SD)
- Use IP address directly — the gateway shows it on boot in serial and in HA as `Gateway IP` sensor
- Best fix: DHCP reservation on your router for a permanent IP

### Web UI shows cached old version

- Hard refresh: `Ctrl+Shift+R` (desktop) or clear site data in phone browser
- Gateway uses boot-time version query string to bust cache on new firmware
- Service worker purges cache on every install

### OTA upload "No response"

- ESP32 is busy with BLE command. Retry — should work on second attempt.
- Check serial: if BLE is actively connecting, wait 30s and retry.

## MQTT Reference

### Published Topics

| Topic | Content | Interval |
|-------|---------|----------|
| `wallbox/status` | r_dat response (power, current, session energy) | 10s |
| `wallbox/realtime` | r_sta response (detailed status, lock, phases) | 30s |
| `wallbox/settings` | Merged settings (auto lock, eco smart, etc.) | 30s |
| `wallbox/response/meter` | r_dca response (voltage, house power, lifetime energy) | 10s |
| `wallbox/response/gateway` | Gateway diagnostics (IP, uptime, heap, RSSI) | 60s |
| `wallbox/availability` | `online` / `offline` | 30s + on change |

### Command Topics

| Topic | Payload | Action |
|-------|---------|--------|
| `wallbox/cmd/charging` | `start` / `stop` | Start/stop charging |
| `wallbox/cmd/current` | `6`-`32` | Set max current (A) |
| `wallbox/cmd/lock` | `lock` / `unlock` | Socket lock |
| `wallbox/cmd/reboot` | `1` | Reboot charger |
| `wallbox/cmd/autolock_enable` | `1`/`0` or `ON`/`OFF` | Auto lock switch |
| `wallbox/cmd/autolock_time` | `60`-`600` | Lock timeout (seconds) |
| `wallbox/cmd/eco_mode` | `Off` / `Solar + Grid` / `Full Green (Solar Only)` | Eco Smart mode |
| `wallbox/cmd/eco_power` | `0`-`100` | Solar target (%) |
| `wallbox/cmd/power_sharing` | `1`/`0` | Dynamic power sharing |
| `wallbox/cmd/phase_switch` | `1`/`0` | Phase switching |
| `wallbox/cmd/halo` | `Off` / `Low` / `Medium` / `High` | LED brightness |
| `wallbox/cmd/timezone` | `Australia/Sydney` (any IANA zone) | Set timezone |
| `wallbox/bapi` | `{"met":"r_dat","par":null}` | Raw BAPI command |

## Development

### Project structure

```
esp32-wallbox/
├── src/
│   ├── main.cpp              # Setup, polling loop, OTA, mDNS
│   ├── wb_ble.cpp            # BLE client, BAPI framing, reconnect logic
│   ├── wb_mqtt.cpp           # MQTT bridge, HA auto-discovery
│   ├── wb_web.cpp            # 4-page web UI + APIs
│   ├── wb_config.cpp         # NVS config manager
│   └── bapi.cpp              # BAPI protocol framing + parser
├── include/
│   ├── wb_ble.h, wb_mqtt.h, wb_web.h, wb_config.h
│   └── bapi.h                # 70+ BAPI method constants
├── docs/
│   └── HOME_ASSISTANT.md     # HA integration guide
├── tools/
│   └── ble_monitor.py        # Serial diagnostic tool
├── partitions_ota.csv        # Dual OTA partition table
└── platformio.ini            # Build config (ESP32-S3 + OTA env)
```

### BAPI Protocol

The BAPI (Bluetooth API) protocol uses JSON over BLE GATT:

```
Request:  EaE | length(1B) | JSON | checksum(1B)
Example:  EaE\x20{"met":"r_dat","par":null,"id":1}\x09

Response: Raw JSON (no framing)
Example:  {"id":1,"r":{"L1":0,"L2":0,"L3":0,"cp":0.0,"cur":32,...}}
```

See `include/bapi.h` for all 70+ method names (r_dat, r_sta, w_cha, w_mxI, s_alo, s_ecos, etc.)

### Tools

- `tools/ble_monitor.py` — Parse serial output, show scan success rate, response times,
  and disconnects. Usage: `python ble_monitor.py --port COM4`
- Importable as module: `from ble_monitor import WallboxMonitor`

## Contributing

Contributions welcome! Please:
1. Open an issue first to discuss significant changes
2. Follow existing code style (C++ follows Arduino conventions)
3. Test on actual hardware before submitting PR
4. Update CHANGELOG.md with your change

## Related Projects

- [Official HA Wallbox integration](https://www.home-assistant.io/integrations/wallbox/) —
  cloud-based, covers session cost / billing history. Complements this gateway (local vs. cloud).
- [`jagheterfredrik/wallbox-ble`](https://github.com/jagheterfredrik/wallbox-ble) — HA
  component for local BLE control of the Pulsar **Plus** (sibling to this project).
- [`jagheterfredrik/wallbox-mqtt-bridge`](https://github.com/jagheterfredrik/wallbox-mqtt-bridge) —
  Runs **on the Wallbox itself** (requires rooting). 1 Hz polling via internal Redis, no
  BLE. Supports Pulsar Plus / Copper SB. Different trade-off than ours: faster updates,
  but you have to root the charger.

## License

MIT — see [LICENSE](LICENSE).

"Wallbox" and "Pulsar" are trademarks of Wallbox Chargers SL. This project is not
affiliated with, endorsed by, or connected to them in any way.
