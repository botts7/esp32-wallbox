# v2.4.0-rc19

Diagnostic improvements after an unexplained reboot on rc18.

## What happened on rc18

botts7's MAX gateway rebooted twice this morning while navigating
the web UI. The in-RAM log buffer wrapped before he could capture
the boot banner, so we couldn't see what reset reason the ESP32
recorded. Two fixes to make that scenario debuggable next time.

## Boot reason capture

`esp_reset_reason()` is now read very early in `setup()` and
persisted to NVS as a ring of the last 10 boots, newest first.

Each entry: `{reason, raw, at}`. Possible reasons:

- `power-on` — fresh power
- `external-reset` — reset pin
- `software (ESP.restart)` — clean reboot we asked for (e.g. OTA)
- `panic (crash)` — exception / hard fault
- `interrupt-watchdog` — interrupt watchdog
- `task-watchdog` — task starved by something blocking
- `watchdog (other)` — other WDT
- `brownout` — power dipped
- `deep-sleep wake` — wake from deep sleep (we don't use this)

Surfaces:

- `GET /api/boot/history` — JSON: `{current, history: [...]}`
- `/info` Charger Info card shows "Last boot: <reason>" inline. Red
  warning prefix if the reason was panic/watchdog/brownout, plus a
  tally of bad boots in recent history if any.

Next time the gateway reboots unexpectedly we'll know exactly why
without needing to be watching the log buffer at the moment of crash.

## Quiet down the rc9 raw-RX log spam

The raw-bytes notification log added in rc9 (to debug peter-mcc's
Plus BGX problem) was firing on every non-EaE BLE packet — fine for
diagnosing what the BGX bridge sends back during initial connect, but
on a healthy MAX connection it meant every multi-packet BAPI
response's continuation packets got logged. The 16 KB in-RAM log
buffer wrapped in ~2 minutes, making post-incident analysis useless.

Fix: track `_seenBapiThisConnection`. Log raw bytes only until we've
received one valid BAPI frame on this connection. Reset in
`_disconnect()` so each fresh connect re-enables the diagnostic at
the moment we actually need it.

Catches the BGX/Plus issue at connect time as designed, stays quiet
once the link is proven good. Log buffer now spans hours instead of
minutes.

## No behaviour change

Pure diagnostic discipline. BLE / MQTT / OTA paths untouched.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11).
Fresh USB installs via `install.json`.

## After upgrading

If a panic / WDT reboot happens, hit `/info` and look at the "Last
boot" line on the Charger Info card. Or `curl /api/boot/history`
for the JSON. We can then dig into what specifically triggered it.
