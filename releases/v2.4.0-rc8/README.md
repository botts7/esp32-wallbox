# v2.4.0-rc8

Diagnostic-accessibility polish on top of rc7. Same Plus protocol fixes,
GATT introspection and OTA safeguards as rc7 — adds in-browser access to
the gateway log, charger notifications surfaced to HA, and full config
export/import.

## In-RAM log buffer

The last ~16 KB of telnet/Serial output is now kept in a circular RAM
buffer accessible without a USB cable:

- **`/api/logs`** — plain text dump of the buffer (auth-gated)
- **`/logs`** — auto-refreshing viewer page, scroll-locked to the latest
  output unless you scroll up

This is the diagnostic you'd normally need a USB-CDC capture for —
useful for catching BLE connect failures, OTA admission rejections,
unexpected reboots, anything the gateway logs.

## Charger notifications → HA

A new poll loop reads `r_not` every 60 seconds while BLE is connected
and publishes a digest to `wallbox/response/notifications`. Two HA
entities auto-discover:

- **Active Notifications** (sensor) — count of currently-active charger
  notifications
- **Latest Notification** (sensor) — text of the most recent one (or
  "None" when clear)

Useful for "your charger is reporting a fault" Home Assistant
automations.

## Config export / import

- **`GET /api/config/export`** returns the NVS config as JSON with all
  passwords/PINs masked as `***`. Safe to share for support or back up.
- **`POST /api/config/import`** restores config from a JSON payload.
  Values equal to `***` are skipped so the existing secret survives —
  meaning you can keep a scrubbed backup in version control and still
  restore the rest of your config from it.

Saves the "redo the entire captive portal" frustration that hits anyone
who needs to factory-reset their gateway.

## Everything from rc7 still in place

All Plus protocol fixes, GATT topology dump, OTA safeguards, charger
presets (MAX / Plus / Copper SB / Quasar / Quasar 2), FW change
tracking, grounding diagnostic, RSSI smoothing, label fixes.

## SHA256

See `SHA256SUMS.txt`.

## Installation

ESP Web Tools manifest at `install.json`. Existing rc7 gateways can
OTA via `/ota` — the rc7 admission guard accepts the upload only when
healthy, so the bricking scenarios that bit rc5/rc6 deployments can't
happen here.
