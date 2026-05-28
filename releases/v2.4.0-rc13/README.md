# v2.4.0-rc13

Disconnect counters — finally instrumented to answer "what's flapping".

## Why this exists

Both my reference MAX gateway and peter-mcc's Plus gateway showed
entities going briefly `unavailable` in Home Assistant. HA's own
Activity log made the symptom visible, but didn't tell us **which**
subsystem flapped (BLE link vs MQTT TCP) or **how often**. Logs in
RAM cover ~30–90 min, not a 24h pattern.

rc13 adds an `wb_diag` module that records each completed
disconnect/reconnect cycle with type and duration, persists the
last 20 events to NVS so reboots don't lose history, and surfaces
the data on `/info`.

## What you get on `/info`

A new **Connection Diagnostics** card showing:

```
BLE reconnects (this boot)    0
MQTT reconnects (this boot)   3 (longest 12s)
Last MQTT reconnect           2h 14m after boot

Recent events (newest first, persisted):
MQTT  at +2h 14m, down 12s
MQTT  at +1h 03m, down 8s
MQTT  at +0h 22m, down 6s
```

Auto-refreshes every 30s. Clear button wipes counters + NVS ring.

## API

- `GET /api/diag/disconnects` — JSON summary + recent events (auth-gated)
- `POST /api/diag/clear` — wipe counters + NVS ring (auth + CSRF gated)

## Implementation notes

- BLE counter hooks into the existing `wasConnected`/`nowConnected`
  edge-trigger in `main.cpp` (one line each side).
- MQTT counter hooks into `WallboxMQTT::loop()` via a new
  `_wasConnected` flag using the same edge-trigger pattern.
- NVS write churn is bounded — we only write on transition events,
  not on every poll. Twenty-event ring caps total write rate.
- Counters are volatile (this-boot); the event ring is persisted.
- No behaviour change in BLE/MQTT paths — purely diagnostic.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11).
Fresh USB installs via `install.json` (ESP Web Tools).

## Once you're on rc13

Leave it running for a day or two. Then go to `/info` and check the
Diagnostics card — if MQTT is flapping but BLE isn't, the issue is
network / broker side. If BLE is flapping, the issue is the link to
the charger.
