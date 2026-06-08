# v2.4.0-rc17

Two small follow-ups after the rc16 Phase-2 ship.

## Schedules page no longer "timeouts"

On rc16, opening the Schedules page right as the BLE task happened to
start a `pollSettings` cycle could cause the web fetch to time out (15s
JS timeout). Root cause: `pollSettings` does 5 sequential BAPI reads,
each defaulted to a 5-second timeout — worst case ~25 s of mutex hold
while waiting for slow settings reads. User-initiated `sendCommand`
(from `/api/command`) was waiting on the same mutex.

rc17 drops the per-call timeout inside `pollSettings` from 5s to 2s.
Settings reads on a healthy link complete well under 1s; 2s is plenty
of happy-path headroom. If a single setting read hangs for >2s,
`pollSettings` gives up on that one entry (it ends up empty in the
merged JSON) and releases the mutex — user-initiated BAPI commands no
longer get starved.

Verified: schedules fetch went from 5.2s (rc16 cold first call) to
0.88s.

## Diagnostics card clarity — "this boot" vs "prior boot" events

`/info` Connection Diagnostics was showing the NVS-persisted event ring
as one undifferentiated list. After multiple OTA reboots, old events
from prior boots looked like recent flaps even though the current boot
might be clean. rc17 splits the list:

- **Events this boot** — full opacity, current uptime-relative
  timestamps
- **From prior boots (NVS-persisted)** — dimmed, phrased as "+Xh Ym of
  that boot" so it's obviously historical

Counters above the list still reflect only the current boot, but now
the event detail matches that mental model.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11).
Fresh USB installs via `install.json`.
