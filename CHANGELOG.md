# Changelog

All notable changes to this project.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [2.2.0] - 2026-05-19

### Added
- **Cost tracking** with time-of-use tariffs. Local tariff editor in Settings → Charger → 💰 Charging Cost. Configure base $/kWh, optional solar (green) rate, and any number of TOU periods (name, rate, day-of-week chips, from/to hours). Top-down match; first tier wins. Stored in `localStorage`. Costs displayed on `/sessions` as Week/Month tiles, $ per day, $ per session (in expanded view), and a new Cost column in CSV export.
- **WebSocket live dashboard** on `:81/`. Server pushes `status` / `meter` / `settings` / `ble` updates as they happen — no polling. Dashboard tiles update in real time. Falls back to HTTP polling automatically.
- **Cache-first rendering** — dashboard tiles paint last-known values from `localStorage` immediately, before any network activity.
- **Charger notifications** surfaced on dashboard — red bell tile + click-through modal with timestamps.
- **BLE health banner** with five tiers (disconnected / very-weak / unresponsive / weak / struggling) using both RSSI and "seconds since last reply".
- New `/api/status` field: `ble_last_activity_s`.

### Fixed
- **All 6 settings panels** (Auto Lock, OCPP, Eco Smart, Timezone, Phase Switch, Halo LED) refuse to render their form if they couldn't read the current state from the charger. Previously fell back to default values that could silently overwrite real config when Saved.
- **Schedule list** distinguishes "empty" (charger has none) from "couldn't load" (BLE blip → Retry button). Was showing "No schedules. Tap + Add New." in both cases.
- **Dashboard `undefined` / `NaN` tiles** — Status / Charging Power / Charging Current / Session Energy / Max Current would render garbage when a field was missing in the WS push payload. Root cause: handler wasn't unwrapping the `{id, r:{…}}` BAPI envelope. Fix: unwrap + per-field numeric guards. If a value isn't a number, keep the last-known value instead of overwriting with junk.
- BLE health/notification polling skips when BLE state is not `connected` — stops the per-minute hammer on a dead link.

## [2.1.2] - 2026-05-19

### Fixed
- Schedule times were ±1 hour off when the user's timezone DST status differed from the hardcoded `Jan 1, 2024` reference date used in `utcToLocal` / `localToUtc`. Both conversion functions now use today's date so the offset is correct year-round (matters most for southern-hemisphere users after their DST ends in April).

## [2.1.1] - 2026-04-26

### Added
- Light / Dark / Auto **theme toggle** (Settings → Charger → 🎨 Theme). Auto follows the OS preference; manual choice persists in localStorage.
- **CSV export** of cached session history (Sessions page → 📥 Export CSV).
- **Clickable heatmap cells** — tap a cell to see every session that delivered energy in that day×hour.
- **Backdrop-blur** on the drilldown modal.

## [2.1.0] - 2026-04-26

### Added
- **/sessions page redesign**: stats tiles (All-time, This Week, This Month) at top, with localStorage-cached session log so revisits are instant.
- **Daily Charging** view groups sessions by day with totals. Click any day to expand and see every individual sub-session (useful for solar/eco-smart charging that pauses/restarts many times).
- **Load older sessions** button paginates through the charger's full history (delta-fetches only new sids on revisit).
- **Schedule CRUD**: per-row Edit and Delete buttons, "+ Add New" form with auto-assigned sid, list refreshes after each operation.
- **Phase Switch panel**: Settings → Settings tab now opens an editable panel (was read-only).
- **Halo LED panel**: Settings → Charger tab — Standby on/off, brightness slider, standby timeout. Matches the official app.
- **Eco Smart panel** now fetches and pre-fills current state (mode + solar power target) and saves both.
- **Release BLE for App** button: temporarily disconnects the gateway from the charger so the official Wallbox app can BLE-connect. Live amber countdown banner with 5-min default. Auto-resumes.
- **Schedule list shows individual sids** and renders edit/delete icons per schedule.
- **Session start–end times** displayed in CHARGER_TZ (no more browser-local drift).

### Fixed
- **FOUC** (flash of unstyled content): body now hidden until `window.load`. Inline html/body background prevents white flash.
- **Heatmap distributes kWh across hours** instead of dumping the whole session into the start hour. 5-min granularity.
- **Heatmap mobile overflow**: now scrolls horizontally inside its card.
- **Heatmap "stuck at 30/30"**: `CHARGER_TZ` was undefined on the `/sessions` page, causing `buildHeatmap` to throw before writing the session list. Now defined on every page that needs it.
- **Session list sorted by timestamp desc** so today's sessions appear at the top regardless of fetch order.
- **Session timestamps render in CHARGER_TZ** (was browser-local), so all browsers see the same time matching the charger's clock.
- **Eco Smart mode mapping** corrected: `esm:1` is **Full Green**, `esm:2` is **Solar + Grid** (was inverted everywhere — web UI, F() display, MQTT discovery template, and HA select command handler).
- **Day bitmask labels** in schedule list: was using Sun-first array but bitmask is Mon = bit 0. Fixed.
- **Session energy unit confusion**: live `r_sta.en` is in 10-Wh (centi-kWh, divide by 100), but historical `r_log.en` is in Wh (divide by 1000). Both now correct.
- **Schedule TZ race condition**: `Q('r_schs')` now awaits `tzReady` before rendering so times never flash as UTC.
- **External CSS in `<head>` instead of bottom of body** — significantly faster page navigation (parallel fetch with HTML parse).

### Notes
- Discharge energy `den` divisor unverified; only matters for Quasar 2 V2H owners.
- BAPI `r_sta.en/gen/grid` are in 10-Wh units; `r_log.en` is in Wh; `r_dca.e` is in Wh. Different scales for different methods.

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

[2.2.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.2.0
[2.1.2]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.2
[2.1.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.1
[2.1.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.0
[2.0.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.1
[2.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.0
[1.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v1.0.0
