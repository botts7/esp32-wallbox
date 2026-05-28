# v2.4.0-rc16

**Phase 2 of the BLE-task split.** Together with rc15 this completes the
architectural fix for "BLE work blocking the web UI / MQTT / WS".

## What rc15 did

Moved the BLE *state machine* — scan / connect / auth / keepalive — to
a dedicated FreeRTOS task. After rc15, a BLE scan (5s) or reconnect
(10s+) could not freeze the web UI. But the *periodic polls* still ran
on the Arduino main loop:

- `pollSettings` was a chain of 5 sequential BAPI reads, up to ~15
  seconds of main-task blocking every 30 seconds
- During those windows, web responses lagged and MQTT keepalive
  starved (rc14 worked around this with `MQTT.loop()` in the yield
  callback)

## What rc16 does

The periodic polls — `pollStatus`, `pollRealtime`, `pollSettings`,
`pollMeter`, `pollNotifications` — now also run inside the BLE
FreeRTOS task. Their results land in cached strings on `WallboxBLE`
protected by a small mutex. Each cache has a monotonic seq counter.

The Arduino main loop's new role for periodic data is: every
iteration, check if a cache's seq has advanced since the last
publish; if so, copy the value out (under mutex, ~ms) and publish to
MQTT / WS. No more BAPI sendCommand calls from the main loop's poll
path.

PubSubClient is not thread-safe, so MQTT publishing stays on the
main task. The mutex split keeps the design clean: **BLE task
produces, main task consumes.**

User-initiated `sendCommand` paths (web `/api/command`, MQTT command
handlers, internal `_authenticate` / `_connect`) are unchanged. The
existing `_cmdMutex` from rc15 serialises against the new BLE-task
polls.

## What this fixes

| Symptom | Status |
|---|---|
| Web pages slow during pollSettings window | **Fixed** — main loop never waits on BLE |
| Variable ping latency to gateway | **Fixed** — main task always responsive |
| OTA chunks timing out during pollSettings | **Fixed** — web handler runs in main task |
| MQTT keepalive starvation (was 95s cycle) | **Fixed in rc14**; rc15/rc16 remove the underlying cause |

## Verification

On the reference MAX gateway running rc16:

- 60-second `/api/status` latency loop: 59 of 60 samples under
  300ms. One outlier at 1.8s (PubSubClient / WiFi radio transient
  — not a poll chain stall).
- `mqtt_reconnects` = 0, `ble_reconnects` = 0 throughout the test.
- Cached status + realtime populate within ~10 seconds of an OTA
  reboot.

## Everything else still in place

All rc7-rc15 features: BGX13P stream-mode unlock (Plus), OTA
safeguards, security hardening, in-RAM log buffer, charger
notifications, config export/import, OTA history, backup/restore UI,
RSSI smoothing, MQTT keepalive fix.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11).
Fresh USB installs via `install.json`.
