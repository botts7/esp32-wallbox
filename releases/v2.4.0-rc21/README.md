# v2.4.0-rc21

Production-ready 2.4.0 release candidate. Six changes:

1. **Root-cause fix for the sustained-load panic** — the BLE yield
   callback was calling `webServer.loop()` while running *inside* a
   web request handler. Arduino WebServer is not re-entrant; the
   nested `handleClient()` clobbered the in-flight `_currentClient`
   and the outer handler then wrote to a freed socket
   (LoadProhibited). This is the same root cause as the long-standing
   "navigation panic" and the sustained-rate panic. Fix is one line
   in `onYield`. Defense in depth: token bucket rate-limit on
   `/api/command` (cap 4, refill 2/s) returning 429 + Retry-After,
   plus a `max_reentry` tripwire on `/api/health` so future
   regressions are observable without a serial console. Verified via
   `scripts/stress_api.py`: 600 requests across sequential + 8-way
   concurrent, zero reboots, max_reentry stayed at 1, heap stable.
2. Root-cause fix for the OTA-during-upload panic that has been
   hitting testers since rc14 (peter-mcc #4) and that I reproduced
   locally on rc20 with a curl multipart POST.
3. Boot-history hardening so the /info "Last boot" badge reflects
   reality after a firmware upgrade.
4. XSS fix in the /config scan-results renderers (BLE device names
   and WiFi SSIDs are both attacker-controllable; they previously
   flowed into innerHTML).
5. Boot / navigation / reconnect overlay with progress bar — protects
   the user from clicking around during the post-reboot BLE handshake
   window. Mode-tracked title so a quick page change doesn't surface
   a misleading "Starting" message; 150 ms deferred show so fast
   page transitions never flash the overlay.
6. JS auto-retry-once wrapper for `/api/command` — honors server-side
   `Retry-After`, retries 429/503 once via the same single-flight
   queue, so tab loaders self-heal during transient BLE busy windows
   without per-call retry code.

## What was crashing

When an OTA upload started, the web/main task called
`WallboxBLE::pause()`, which synchronously called `_disconnect()` —
tearing down the NimBLE client object that the BLE task on Core 1
still owned and could be reading mid-frame in a notification callback.
Use-after-free on the client pointer reliably panicked the gateway.

This explains:

- peter-mcc's "OTA still reboots it" reports since rc14
- My own MAX gateway panicking on every curl-driven OTA on rc20

rc20's JS fetch limiter mitigated browser-driven concurrent loads but
did nothing for this underlying C++ task-ownership race, which is why
curl-driven OTA still panicked.

## What rc21 changes

### `WallboxBLE::pause()` — flag-only, no cross-task work

`pause()` now only sets `_pausedUntil`. The BLE task observes the flag
on its next ~20ms tick and tears down NimBLE state from its own task
context. No cross-task pointer ownership transfer, no race.

Caller-facing behaviour is unchanged — `isPaused()` still returns true
immediately. The only difference is the BLE disconnect itself happens
on the BLE task's next loop iteration (~20ms), not synchronously
inside `pause()`.

### Boot history hardening

Each boot record now carries two extra fields:

- **`fw`** — the firmware version that wrote the record (from
  `WB_VERSION`, set by `git describe`). Lets the /info badge count
  panics where `fw == current_fw` as "bad boots on this firmware" and
  quietly tag older-fw panics as "from older firmware". Fixes the
  alarming "7 bad boots in recent history" badge that surfaced after
  rc20 upgrade — those panics were all carried over from rc18/rc19/
  rc20-dev testing, not from production firmware.
- **`at`** — wall-clock epoch when first observed by SNTP. Means
  "stable since X" claims are now provable.

Entries written before rc21 have neither field; the badge treats them
as "older firmware" and quietly dims them.

### SNTP configured

The project had no time sync configured — `time(NULL)` previously
always returned 0. rc21 calls `configTime(0, 0, "pool.ntp.org")` after
WiFi connects so the `at` field can be patched once sync lands
(typically ~1–3s after WiFi up).

### XSS fix in /config scan results

Background security review flagged two XSS sinks in the JS that
renders BLE-scan and WiFi-scan responses on the /config page. Both
data sources are attacker-controllable from the radio environment:

- **BLE device names** — any radio within ~30m can advertise an
  arbitrary string. A name like `<img src=x onerror=...>` previously
  flowed into innerHTML and would run in the admin-auth context of
  whoever opened the Scan-for-Chargers card. With OTA in scope, that
  was a full takeover primitive.
- **WiFi SSIDs** — same pattern. Trigger is the initial WiFi-setup
  flow (gateway in AP mode), which is when the operator is most
  exposed because they're explicitly looking at hostile-radio output.

Fix: both result lists now render via `document.createElement` +
`textContent` + `addEventListener`. Attacker-controllable strings can
no longer parse as HTML — the only thing that ever sees the raw SSID
or device name string is a Text node. No new dependencies, no
escape-helper to get wrong.

Other innerHTML sites in the file (settings tabs, OCPP / Eco / Halo
forms, toast etc.) read from BAPI responses — i.e. data sourced from
the admin's own charger over an authenticated BLE link — and stay
as-is. That data is trusted in our threat model and the refactor cost
is not justified for this release.

### Navigation panic — root-causal fix

The rc20 README documented "rapid back-to-back navigation can panic"
as a known limitation deferred to 2.5.x. Investigation here showed it
actually fires on a single user opening /settings, not just
deliberate rapid navigation:

- Browser issues 7 parallel requests to render /settings: HTML page +
  /style.css + /app.js + /manifest.json + /sw.js + on-load
  `fetch('/api/status')` + on-load `r_schs` BAPI call (5–15 s
  blocking).
- Each concurrent ESP32 HTTP response allocates ~10–20 KB of heap.
  heap_min_ever dipped from 128 KB to 59 KB under that pattern;
  malloc failures begin around 30 KB free.
- The blocking `r_schs` BAPI call extended the window during which
  all the static-asset responses were holding their buffers.

Two targeted fixes, no architectural rework:

- `/style.css` and `/app.js` now serve
  `Cache-Control: public, max-age=31536000, immutable`. Both URLs
  already carry `?v=<buildVer>` cache-busters so caching forever is
  safe. Eliminates two of the parallel asset fetches on every
  navigation after the first.
- The on-load fetches on /settings (`/api/status` and
  `loadSchedules()`) moved from script-parse time to
  `window.addEventListener('load', ...)`. The heavy BAPI call no
  longer races the static-asset GETs.

Stress-tested: 5 rounds × 7-way parallel /settings storm + 3 rounds ×
5-way rapid-nav across all major pages. Zero panics. heap_min_ever
bottoms at 50 KB and stays there. 200 s soak afterwards: heap
recovers to 142 KB free baseline.

## Effects

| Scenario | Before rc21 | After rc21 |
|---|---|---|
| OTA upload via curl | Panic, gateway reboots into old firmware | Clean upload + reboot |
| OTA upload via browser | Mostly fine (JS limiter helped) | Clean (race no longer reachable at all) |
| Post-upgrade /info badge | "(7 bad boots in recent history)" red | "(6 from older firmware)" dim grey |
| Boot history `at` field | Always 0 | Real epoch once NTP syncs |

## Verification

Tested on local MAX gateway:

- **Three consecutive clean OTA cycles** with the rc21 firmware in
  place. rc20 in the same scenario reliably panicked on the first
  attempt.
- `/api/boot/history` shows `current_fw: "v2.4.0-rc21"` and the new
  entry carries both `fw` and `at` fields.
- NTP sync patches `at` to a real epoch within ~60s of boot.
- 20 parallel `/api/charger` reads all return 200 (no regression to
  the rc20 limiter).
- `/sessions` and `/info` both render and respond < 500ms.
- BLE auto-reconnects after the OTA-window pause expires.

## SHA256

See `SHA256SUMS.txt`.

## Installation

**Existing rc7+ gateways**: OTA via `/ota` (auth required since rc11).
With rc21 in place, OTA is reliable end-to-end. For testers stuck on
rc14/rc20 hitting the OTA panic, USB-flash directly:

```
python -m esptool --chip esp32s3 --port COMx --baud 921600 write_flash \
  0x0     bootloader.bin \
  0x8000  partitions.bin \
  0x10000 wallbox-gateway-esp32s3-v2.4.0-rc21.bin
```

**Fresh installs**: use `install.json` with ESP Web Tools.

## After upgrading

The /info "Last boot" line will read `software (ESP.restart)` and any
older panics will be dimmed as "(N from older firmware)" rather than
red-flagged. As the boot ring rolls over with fresh entries, older-fw
counts decay to zero on their own. No manual NVS clear needed.
