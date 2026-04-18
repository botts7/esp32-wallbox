# Changelog

All notable changes to this project.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [2.0.1] - 2026-04-18

### Fixed
- Charging current was displayed in raw deciamps (98A showed when actually 9.8A). All currents now divided by 10.
- Session/green/grid energies were displayed in raw watt-hours. Now divided by 1000 for kWh.
- Session Energy tile showed 0 because it was reading `den` (V2H discharge) instead of `en` (session energy).
- WiFi scan result dropdown didn't populate — replaced with visible clickable result list.
- SSID was a locked `<select>` — now a text input with scan suggestions, users can type manually.
- Nav bar items had uneven widths — now use `flex:1 1 0` for equal distribution.
- Button sizes were inconsistent (outline buttons were 2px narrower due to borders). Fixed with `box-sizing:border-box`.
- Phone Chrome aggressive caching — disabled cache headers on CSS/JS, added version query string, service worker purges cache on install.
- Multiple null pointer errors in save functions — replaced result-div writes with toast notifications.
- `sessionEnergy` → `(value_json.r.en / 1000)` in MQTT template.
- `current_lX` HA templates divided by 10.

### Added
- Gateway IP sensor in HA (useful when DHCP lease changes).
- `House Power` / `House Current` HA entities (renamed from Grid Power/Meter Current for clarity).
- `Grid Energy`, `Green Energy`, `Discharge Energy (V2H)` session HA entities.
- `Lifetime Energy` from MID meter for HA Energy Dashboard.
- Time-of-use cost tracking documentation in HA docs (2-tier and 4-tier examples).
- mDNS retry logic (3 attempts) and TXT records for better discovery.
- Clear labels: "Charging Power", "Charging Current", "Session Energy" vs "House Power".

### Changed
- Dashboard tile values rounded to 2 decimals (was showing 2.3520000001 kW).

## [2.0.0] - 2026-04-17

### Added
- **Phase A:**
  - HA native entities for all charger settings (switches, numbers, selects)
  - Auto Lock, Eco Smart Mode, Power Sharing, Phase Switch, Halo LED, Timezone
  - `pollSettings()` merges all config reads into `wallbox/settings` topic
  - Command handlers for all new HA entities (convert HA payloads → BAPI JSON)
  - Slider auto-sends with 300ms debounce (removed "Set" button)
  - Session history browser (last 20 sessions via `r_log`)
  - Factory reset uses toast-style modal instead of browser `confirm()`
  - Weekly session heatmap page (`/sessions`) — day × hour grid, kWh intensity
  - Charger Details card on Info page (manufacturer, model, BLE firmware from GATT)
  - HA diagnostic sensors: charger_name, manufacturer, model, BLE firmware
  - Responsive dashboard tiles (`repeat(auto-fit, minmax(140px, 1fr))`)
  - PWA manifest with inline SVG icon — installable on iOS/Android
  - Minimal service worker
  - Mobile tab scroll fade
  - Home Assistant integration docs (`docs/HOME_ASSISTANT.md`)

- **Phase B (Security):**
  - Web authentication (optional, configurable in NVS)
  - Rate limiting: 1s delay per failed login, 30s lockout after 5 attempts
  - CSRF token protection on POST endpoints
  - Custom 404 page with dark theme
  - `ble_monitor.py` refactored to `WallboxMonitor` importable class

- **Phase C:**
  - Weekly sessions heatmap page

### Changed
- Settings page reorganized into 4 tabs: Schedules, Power, Security, Charger
- Single-panel UI: all editors render into one result area per tab (no duplicate cards)

## [1.0.0] - 2026-04-16

### Initial Release

- **BLE Gateway**: ESP32-S3 with NimBLE, connects to Wallbox BAPI protocol
- **WiFi coexistence**: `updateConnParams`, ping keepalive, smart scan cache
- **MQTT**: Home Assistant auto-discovery with 20+ entities
- **Web UI**: 4-page navigation (Dashboard, Settings, Config, Info)
  - Dashboard: live status, controls (start/stop/lock), current slider
  - Settings: schedules, eco smart, OCPP, auto lock
  - Config: WiFi, MQTT, BLE, web auth, advanced UUIDs
  - Info: gateway stats, raw BAPI tool
- **OTA**: dual partition table with automatic rollback, web upload page
- **Configuration**: NVS persistence, AP captive portal for first-boot setup
- **mDNS**: `wallbox-gw.local` hostname
- **MIT License** with Wallbox trademark disclaimer
- **Confirmed working**: Wallbox Pulsar MAX with u-blox NINA-B22 BLE radio

[2.0.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.1
[2.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.0
[1.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v1.0.0
