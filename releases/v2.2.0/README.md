# Wallbox Gateway v2.2.0

Significant feature + reliability release on top of [v2.1.2](../v2.1.2/README.md).

## Highlights

### 💸 Cost tracking with time-of-use tariffs
- New **💰 Charging Cost** panel (Settings → Charger)
- Configure base $/kWh, optional solar (green) rate, and any number of TOU periods (name, rate, day-of-week chips, from/to hours)
- Top-down match — first matching tier wins
- Costs computed in JS, stored in `localStorage` (no firmware bloat)
- Displayed throughout `/sessions`: new **Week / Month cost** tiles, **$ per day** in Daily Charging, **$ per session** when a day is expanded, **Cost** column in CSV export
- Solar (green) kWh priced separately at the green rate
- HA users: pair with HA's Energy dashboard for native cost statistics

### ⚡ Live WebSocket dashboard
- New WS server on `:81/` — browser opens a single persistent connection instead of HTTP polling
- Server pushes `status` / `meter` / `settings` / `ble` updates as they happen
- Dashboard tiles update in real time as fresh data arrives from BLE
- Cache-first rendering — last-known values painted instantly on page load (from `localStorage`), no spinners
- Falls back to HTTP polling automatically if WS doesn't connect
- `<html data-ws="1">` set when connected (for CSS hooks)

### 🔔 Charger notifications surfaced on dashboard
- Red bell tile appears at top of dashboard when the charger has any internal notifications/errors
- Tap → blurred-background modal lists each notification with timestamp
- Polls every 60 s, but only when BLE is connected (no blocking when link is flaky)

### 📡 BLE health & "struggling" detection
- New always-on health banner at top of dashboard:
  - 🔴 **Disconnected** — "gateway can't reach charger"
  - 🔴 **Very weak signal** (RSSI < −90) — "move ESP32 closer"
  - 🔴 **Unresponsive** — connected but no replies > 120 s
  - 🟡 **Weak signal** (RSSI < −80) — performance may suffer
  - 🟡 **Struggling** — connected but no replies > 60 s
- New `/api/status` field: `ble_last_activity_s` (seconds since last successful BAPI reply)

### 🛡️ Settings panels won't overwrite real config
- **All 6 panels** (Auto Lock, OCPP, Eco Smart, Timezone, Phase Switch, Halo LED) now refuse to render the form if they couldn't read the current state from the charger
- Previously: panel pre-filled with default values when BLE call failed → user could click Save and silently overwrite real config with defaults
- Now: shows "Couldn't read X state — [Retry]" instead

### 🛡️ Schedule list distinguishes empty vs failed-fetch
- Was showing "No schedules. Tap + Add New." regardless of whether the charger actually had no schedules OR the BLE call failed
- Now correctly identifies: empty (charger has none) vs garbage shape (BLE blip → Retry button)

### 🐛 Dashboard "undefined/NaN" bugs squashed
- Status / Power / Current / Session Energy / Max Current would render as "Code undefined", "NaN kW", "undefinedA" etc. when a field was missing in the WS push payload
- Root cause: WS handler wasn't unwrapping the `{id, r:{…}}` BAPI envelope
- Fix: handler unwraps `.r`, **and** every numeric tile is now guarded — keeps last-known value if the new value isn't a number

### Other
- BLE state polled separately from BAPI commands — UI stays responsive when link is in error
- Heatmap modal has backdrop blur for better readability (carried from v2.1.1)
- Many small UX hardenings (timestamps in CHARGER_TZ, graceful degradation, etc.)

## Files

| File | SHA256 |
|------|--------|
| `wallbox-gateway-esp32s3-v2.2.0.bin` | `31990bc0bd4a66b0bf0518f28d495d5134ca8c2ef1fd217087e7935d53df5f5d` |
| `bootloader.bin` | `1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343` |
| `partitions.bin` | `2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e` |

## Install

**Upgrading from any v2.x:** open `http://wallbox-gw.local/ota` and upload `wallbox-gateway-esp32s3-v2.2.0.bin`. Settings, WiFi, MQTT, schedules — all preserved.

**Fresh install:** see the [v2.1.0 README](../v2.1.0/README.md#installation).

## After upgrading

- The dashboard will now show last-known values from cache instantly. If you want a fresh start, clear `localStorage` for the gateway in your browser.
- To enable cost tracking: Settings → Charger → **💰 Charging Cost** → tick "Enable cost tracking", configure base + tariff periods, save.
- HA users: the official Energy dashboard remains the recommended path for long-term cost statistics — this gateway's local cost view is the web-UI counterpart.
