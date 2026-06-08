# v2.4.0-rc12

Two small UX wins requested by peter-mcc on issue #4 — both make the
post-reboot debug-trace capture workflow much easier.

## Reboot the gateway from the web (without wiping config)

Previously the only way to reboot was Save & Reboot (which writes NVS)
or Factory Reset (which wipes it). Both are wrong for the "I just want
a fresh boot trace" case.

rc12 adds:

- `POST /api/reboot` — auth + CSRF gated, no NVS touch, just calls
  `ESP.restart()`.
- **Reboot Gateway** button on `/config` (next to Factory Reset).
- **Reboot & capture boot trace** button at the top of `/logs` — the
  one Peter actually asked for. Click it and the page stays open,
  showing the trace as it accumulates after boot.

## Logs in the nav bar

`/logs` now has its own icon in the bottom navigation bar (Dashboard
→ Settings → Config → Info → **Logs**). Same buffer as before — last
~16 KB of telnet/Serial output, kept in RAM.

## Log viewer polish

The viewer was thin before; with the new reboot workflow it gets more
work:

- Online/offline indicator next to the title. Goes red after two
  consecutive failed polls — makes the post-reboot window obvious
  instead of the page silently freezing.
- **Copy** button — clipboard the whole buffer for pasting into an
  issue.
- **Download** button — `/api/logs?download` saves as
  `wallbox-log.txt`.
- Poll interval 5 s → 3 s so the boot trace appears faster.
- Help text clarifies that the buffer survives page reloads but is
  wiped on actual gateway reboot.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota`. Make sure web auth is logged in
(rc11+ requires auth on the OTA endpoint).
