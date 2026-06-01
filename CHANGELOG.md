# Changelog

All notable changes to this project.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [2.4.3] - 2026-06-02

Tightens the `loop_max_ms` tripwire so it stops false-positiving on
sync MQTT/BLE reconnect blocking. Surfaced by @peter-mcc shortly after
2.4.2 shipped.

### Fixed

- **`loop_max_ms` saturated by MQTT reconnect blocking.** When the
  MQTT broker briefly goes away — an HA add-on reload, a router
  hiccup — sync `PubSubClient::connect()` blocks the gateway's main
  task for ~15 s per attempt while it retries. The tripwire
  faithfully measured that as a 30-40 s loop gap, but it wasn't the
  kind of *unprovoked* runtime wedge the metric was built to catch.
  Once the value saturated, any real wedge underneath was invisible.
- New `wb_diag::extendLoopMaxGate(graceMs)` API. Called automatically
  from `reportReconnect()` (the existing single-source-of-truth for
  tracked reconnect events) — every successful BLE or MQTT reconnect
  pushes a 30 s grace window forward. The main loop's gap tracker
  consults `loopMaxGateActive(now)` and skips recording during that
  window.
- Overlapping reconnects only extend the window, never shorten it.
  The gate auto-clears on expiry so the next event re-arms cleanly.

### Layered filters now stacked on `loop_max_ms`

What lands in the metric after **all three** filters fire is a
genuine unprovoked wedge worth investigating:

1. First 60 s of uptime (boot-phase MQTT discovery flood — 2.4.1)
2. 30 s after any tracked BLE/MQTT reconnect (this release)
3. The trivial "first-iteration timestamp is meaningless" check

### Live-validation

Confirmed on the maintainer's MAX by stopping and restarting the
Mosquitto broker:

  Before: loop_max_ms = 31 ms, mqtt_reconnects = 0
  After:  loop_max_ms = 265 ms, mqtt_reconnects = 1, longest 10 s

Without the gate that 10 s outage would have produced 10 000+ ms.
With the gate it surfaces as 265 ms — comfortably under the 500 ms
yellow threshold. Heap, BLE, and 2.4.2 fields all intact through and
after the test.

## [2.4.2] - 2026-06-01

Follow-up release driven by @peter-mcc's testing of 2.4.1 on his
Pulsar Plus. Two real bugs and a quiet drift between the discovery
helpers — all fixed live on the maintainer's MAX before tagging.

### Fixed

- **Total Charging Sessions sensor was reading the wrong field.** 2.4.1
  surfaced `r_ses.size` which is actually the log-buffer capacity
  sentinel (99 999 on MAX, missing on Plus) — not a lifetime count.
  Switched to `r_ses.last` (verified 233 on the maintainer's MAX);
  kept `size` as a fallback but only when in a plausible range so the
  sentinel can't sneak in. When neither field yields a real value
  (some Plus firmwares), `chg_sessions` publishes JSON `null` so the
  HA sensor goes unavailable instead of sticking at 0 or -1 forever.
- **HA Device-page firmware label was hardcoded.** 2.4.1 wrote
  `dev["sw_version"] = "6.11.16"` in the discovery device block, so
  every Pulsar — Plus, Quasar, Copper, anything that isn't a MAX —
  showed `6.11.16` in HA even though the per-entity Charger Firmware
  sensor was correct. Now driven by `wallboxBLE.chargerAppFirmware()`
  with a `WB_VERSION` fallback until `fw_v_` has been read. BLE init
  raises a one-shot `_discoveryStale` flag the first time the value
  populates, and the main loop triggers `wallboxMQTT.sendDiscovery()`
  once to update HA in place.

### Changed

- **Single source of truth for the HA discovery device block.** Six
  discovery helpers (entity / switch / number / button / select / the
  inline car-connected binary_sensor) each built the device block
  inline and had drifted — only the entity helper carried `sw_version`,
  the others didn't. Consolidated into `populateDeviceBlock()`. Every
  payload now carries the same complete block:
    - `identifiers`, `name`, `manufacturer`, `model`, `sw_version`
    - `connections: [["mac", <wifimac>]]` so the HA Device page header
      shows the gateway's MAC alongside the firmware label
    - `configuration_url: http://<gw-ip>/` so the HA Device page header
      gets a "Visit" button deep-linking to the gateway dashboard

### Migration notes

If you're upgrading from 2.4.1 and your HA Device page still says
"Connected via Unnamed device" or shows stale entities (the
`chg_net_rssi` sensor from the dev build), the cleanest reset is to
delete the Wallbox device in HA (Settings → Devices → device → Delete)
and reboot the gateway via `/info` → Reboot Gateway. The fresh
discovery payloads will rebuild the device with everything correct;
HA stops linking it under the MQTT broker hub once the device block
carries a full `connections` array.

## [2.4.1] - 2026-05-31

### Added

- **Charger firmware + project identity.** New `fw_v_` BAPI read at BLE init
  pulls the charger's app firmware and project string (e.g. `prj15-pulsar-max`).
  Surfaced on `/info` (new "Firmware" section under Charger Details) and as a
  separate **Charger Firmware** entity in Home Assistant — previously only the
  BLE module's firmware was visible, which routinely got mistaken for the
  charger's app version.
- **Model auto-detection.** `inferredModel()` maps the charger's project
  string to a friendly name (Pulsar MAX / Plus / Copper SB / Quasar / Quasar 2).
- **Total charging sessions counter.** Reads `r_ses` once per BLE connection,
  exposes as `chg_sessions` on `/api/status`, ships a `total_increasing`
  sensor to HA, and renders as a badge next to the Charging Sessions page
  header.
- **Power Boost (ICP) limit.** `r_hsh` BAPI read; surfaced as `chg_power_boost`
  and an HA sensor with `A` unit.
- **Discrete lock state.** `r_lck` returns 0=unlocked / 1=locked; published as
  a `binary_sensor` with `device_class: lock`.
- **Charger-side network detail.** `gnsta` BAPI read; the charger's own
  WiFi SSID / IP / RSSI are now exposed alongside the gateway's network
  in a "Charger Network" section on `/info`.
- **Gateway WiFi panel.** `/info` Charger Details card now also shows the
  ESP32's own SSID / IP / RSSI in a "Gateway WiFi" section.
- **Loop-wedge tripwire (`loop_max_ms`).** Tracks the longest gap between
  consecutive main `loop()` iterations since boot. Healthy values are under
  ~200 ms in steady state; multi-second values indicate a wedge of the kind
  observed during long HTTP responses in earlier RCs. Surfaced on
  `/api/health`, MQTT, and the new "Runtime Health" card in `/info`'s
  Diagnostics view.

### Changed — OTA hardening

- **503 + Retry-After on admission rejection.** The OTA upload path no
  longer responds with a generic 500 when admission denies an upload — it
  emits a proper `503` with `Retry-After` indicating when to retry (the
  exact uptime crossover, plus a small cushion).
- **Browser auto-retry on 503.** `/ota` page's upload script parses
  `Retry-After`, runs a live countdown, and automatically re-fires the
  upload once. Eliminates the manual re-click that previously followed
  every just-after-reboot OTA attempt.
- **Relaxed admission window after first successful OTA.** A device that
  has completed one OTA + reached healthy state on the new firmware
  flips a NVS-backed `ota_proven` flag. Subsequent admission checks drop
  the minimum uptime threshold from 60 s to 15 s.
- **RAII watchdog scope (`wb_watchdog` module).** Backport of the
  ESPHome PR #16138 pattern. `wb_wdt::extendTo(60)` / `wb_wdt::restore()`
  guarantee the Task WDT is restored to its default 5 s timeout even
  when the OTA path early-returns on error — previously a failed OTA
  could leave the WDT permanently relaxed.
- **Optional MD5 verification.** When clients (curl, HA OTA scripts) send
  an `X-Firmware-MD5` header, the Update library streams the hash
  alongside the partition writes and refuses to commit on mismatch.
  Browser uploads continue to skip this to avoid the multi-second
  hash step on 1.5 MB files.
- **`ota_proven` and `ota_min_uptime` exposed on `/api/health`** so
  testers can confirm the relaxed admission window engaged without
  scraping logs.

### Documentation

- **`RELEASING.md`** documents the four pre-release gates introduced
  after the rc23/24/25 cycle: fresh-install smoke, browser OTA, stress +
  tripwires, and read-only BAPI probe. A tagged non-RC release must pass
  all four.

### Background / Why

A run of release candidates (rc23 → rc25) shipped fixes that passed the
maintainer's curl-based tests but failed when @peter-mcc actually exercised
them in a browser. 2.4.1 closes the surface gap: every new diagnostic,
every OTA path, and every charger-side exposure was validated end-to-end on
a live Pulsar MAX before tagging.

## [2.3.0] - 2026-05-22

### 🙏 Community contributions

- **BLE SMP pairing for Wallbox firmware ≥ 6.11.26** — newer charger firmware
  requires an encrypted BLE link before allowing CCCD writes (notification
  subscription). The gateway now tries the plain `registerForNotify` first
  (backwards-compatible with older firmware) and falls back to SMP passkey
  pairing using the configured BAPI PIN if rejected. Tested on FW 6.11.16 (no
  fallback needed) — anyone on 6.11.26+ should now be able to use the gateway.
  Thanks to **[@benvanmierloo](https://github.com/benvanmierloo)** for the
  contribution! ([#1](https://github.com/botts7/esp32-wallbox/pull/1))
- **Telnet log server on port 23** — all `Serial` output is now also streamed
  to up to two LAN telnet clients (`telnet wallbox-gw.local`). Service is
  advertised via mDNS. Zero overhead when no client is connected. Also by
  **[@benvanmierloo](https://github.com/benvanmierloo)**.

### Added

- **Solar savings** on the `/sessions` stats tiles. Week / Month cost tiles now
  show an additional ☀ "saved $X" line — the dollar value of solar (green) kWh
  vs what grid would have cost at your tariff. Configurable green rate (default
  $0 = "free" solar; set to your feed-in tariff to see opportunity cost).
- **Notifications panel** (Settings → Charger → 🔔 Notifications) — detects
  that the gateway runs on plain HTTP (no Notification API access) and shows a
  ready-to-paste Home Assistant automation snippet that gives proper push
  notifications via the HA Companion app, using our existing
  `sensor.wallbox_pulsar_max_status` entity.

### Fixed

- **Page reveal** now triggers on `DOMContentLoaded` instead of `window.load` —
  much faster perceived load when the gateway is busy talking to a weak BLE
  link (was waiting on every CSS/JS asset before showing anything).

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

[2.3.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.3.0
[2.2.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.2.0
[2.1.2]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.2
[2.1.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.1
[2.1.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.0
[2.0.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.1
[2.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.0
[1.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v1.0.0
