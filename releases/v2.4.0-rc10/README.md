# v2.4.0-rc10

Diagnostic + UX polish on top of rc9 — no protocol or OTA-safety changes.

## OTA history log

The gateway now persists the last 5 OTA attempts to NVS and surfaces them
on the `/info` page. Each entry records:

- Timestamp (uptime when the OTA finished)
- The version that was running when the attempt happened (`from`)
- Bytes received
- Outcome (success / aborted / Update.end-failed)
- A short reason string if it failed

Useful for:
- Catching flapping-OTA patterns (a release that boots-but-doesn't-stay-healthy)
- Evidence when a deployment misbehaves after a flash
- Verifying that the rc7+ admission guard is actually rejecting bad uploads (which appear as failed entries with a clear reason)

API: `GET /api/ota/history` returns the array (newest first), auth-gated.

## Backup & Restore UI on /config

The `/api/config/export` and `/api/config/import` endpoints shipped in
rc8 — but the only way to use them was `curl`. rc10 adds a proper card
to `/config` with:

- **Download config** button — saves `wallbox-config.json` to your
  browser's downloads folder. Passwords/PINs are masked as `***` in the
  download (so it's safe to share for support).
- **Restore from file** picker + button — uploads a previously saved
  JSON back to the gateway and reboots. Fields equal to `***` are
  skipped on import, preserving any existing secrets so you can keep a
  scrubbed backup in version control.

Solves the "I just had to redo the entire captive portal" pain after a
factory reset.

## Daily / weekly / monthly energy totals (HA-side, no firmware change)

`sensor.wallbox_meter_total_energy` already publishes lifetime kWh as
`total_increasing`, which is exactly what HA's Energy dashboard and
Utility Meter helpers need. The recipe for daily/weekly/monthly cycles
is already documented in
[`docs/HOME_ASSISTANT.md`](../../docs/HOME_ASSISTANT.md) — search for
`utility_meter`.

## Everything from rc9 still in place

- BGX13P STREAM_MODE switch for Pulsar Plus
- Raw-RX diagnostic logging for non-BAPI frames
- Adaptive PIN auth probe
- `r_dat` keepalive on Plus
- Plus 19-state status enum
- GATT topology dump at connect
- OTA admission guard (uptime + WiFi + healthy gate)
- In-RAM log buffer + `/api/logs` + `/logs` viewer
- `r_not` charger notifications → HA
- Config export / import (rc10 just adds the UI)
- Charger presets (MAX / Plus / Copper SB / Quasar / Quasar 2)
- FW change tracking
- Grounding diagnostic
- RSSI smoothing + single-source

## SHA256

See `SHA256SUMS.txt`.

## Installation

ESP Web Tools manifest at `install.json`. Existing rc7+ gateways can OTA
via `/ota` — the admission guard accepts the upload only when healthy.
