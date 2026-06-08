# v2.4.0-rc20

Panic mitigation under web load. Significant stability improvement on
the /sessions page and rapid web UI use.

## What was crashing

botts7 reported the gateway rebooting when navigating the web UI on
rc18. With chrome-devtools-mcp driving a real Chrome session against
the gateway in this development cycle, the panics became reliably
reproducible:

- Loading `/sessions` fired 10+ parallel `r_log` BAPI requests
  (one per session id), each waiting up to 6s on the BAPI mutex —
  the queue piled up faster than the ESP32 WebServer could drain it.
- Every 3rd request would return `ERR_EMPTY_RESPONSE`. Sometimes
  the gateway panicked outright.
- 20 parallel curl `r_log` calls reliably panicked rc19.

The new `esp_reset_reason()` capture confirmed: `panic (crash)`,
not `task-watchdog` or `brownout`. Pure C++ runtime fault under
load.

## What rc20 changes

### Boot reason capture (carried from rc19)

- `GET /api/boot/history` — last 10 boots with reason
- `/info` Charger Info card shows "Last boot: <reason>" inline with
  a red warning if it was `panic` / `watchdog` / `brownout`

### Runtime diagnostics

- `GET /api/diag/runtime` returns heap stats + per-task stack
  high-water marks

### JS fetch limiter (global)

- Installed in `htmlHead` so it applies on every page
- Wraps `window.fetch` with a queue that caps **concurrent
  in-flight requests at 1**
- Rest of the same page's fetches queue. Per-request latency on the
  happy path unchanged

### Server-side /api/command rate limit

- Tracks in-flight count via a static counter
- Caps at 2 in-flight commands; rejects the rest with `503 {error:busy,
  retry:true}`
- Stops the BAPI mutex queue from piling up so deep it exhausts LWIP /
  heap resources

### /info chained fetches

- `loadGW` → `loadBootReason` → `loadOtaHistory` → `loadDiag`
  sequentially instead of firing 4 in parallel

## Effects

| Scenario | Before rc20 | After rc20 |
|---|---|---|
| Load /sessions once | Every 3rd r_log → ERR_EMPTY_RESPONSE | All clean |
| 20 parallel r_log curl calls | Reliable panic | Survives, most returns 503 |
| Sit on /info for 30s | Stable | Stable |
| Single navigation /info → /sessions | OK | OK |
| Rapid 5+ page navigation in <3s | Panic | **Still panics — known issue** |

## Known remaining issue

Rapid back-to-back page navigation (5+ pages in under 3 seconds) can
still trigger a panic. Root cause appears to be TCP socket
exhaustion when WebSocket close+reopen + static asset fetches +
queued BAPI calls overlap. Proper fix requires converting BAPI
handler to a non-blocking async pattern (return a request ID, browser
polls for result). Tracked as rc21 candidate work.

For typical use — one page at a time, normal browsing speed — the
gateway is now stable.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota`. Fresh USB installs via
`install.json`.
