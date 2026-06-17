# Changelog

All notable changes to this project.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [3.0.3] - 2026-06-17

Patch release — fixes two Home Assistant MQTT-discovery issues reported in #14.

### Fixed

- **Timezone list now covers all major IANA zones.** The charger-time
  timezone select only offered 16 curated zones, so many regions (e.g.
  `Europe/Amsterdam`) couldn't be selected at all. Expanded to ~75 IANA
  zones and made the discovery option count self-sizing so it can't silently
  truncate again. (#14)
- **Session-energy sensors no longer error in Home Assistant.**
  `green_energy_session` and `grid_energy_session` were published with
  `state_class: measurement`, which HA rejects for an `energy` device class
  (the entities showed as unavailable/errored). Changed to
  `total_increasing`. (#14)

## [3.0.2] - 2026-06-17

Patch release — fixes a post-OTA stale-page problem that could make a fixed
gateway look broken (e.g. schedule delete appearing to fail because the
browser was running pre-update JS).

### Fixed

- **HTML pages are no longer cached.** They were served with no
  `Cache-Control`, so the browser / installed PWA / service worker cached
  them and kept serving stale JS after an OTA — a gateway already updated to
  the fixed firmware would still run the old code (most visibly, the old
  broken schedule delete). Pages now send `Cache-Control: no-store`.
- **Stale open tabs auto-reload after an OTA.** The page compares its
  baked-in firmware version against the gateway's live version (on focus +
  every 60 s) and reloads if they differ, so an already-open tab picks up
  new firmware without a manual hard-refresh. (No-ops on matching firmware.)
- **Schedule-delete feedback.** Delete no longer optimistically drops the
  row (which made a failed/timed-out delete look like it half-worked). The
  row stays until the charger confirms; success/error/timeout each show a
  clear toast and reload to the true state, with a specific "charger busy,
  try again" message on timeout.

## [3.0.1] - 2026-06-16

Patch release: schedule delete finally works, a real one-click web
installer, plus robustness + diagnostics fixes on top of 3.0.0.

### Fixed

- **Schedule delete.** Deleting a schedule now uses the charger's native
  `clr_sch` with `{"sid":[N]}` (the `sid` key takes an array) instead of the
  old clear-all-then-rewrite, which silently failed on charger fw 6.11.x.
  Single delete and delete-all both work; verified on hardware.
- **Web installer.** Replaced the inline README install button (GitHub
  strips the script, so it never rendered) with a hosted GitHub Pages
  installer, and serve the manifest + firmware same-origin to fix a CORS
  "Failed to download manifest" error. ESP32-S3 boards only.
- **Boot overlay** no longer gated on the BLE-to-charger link, so a gateway
  that's up on WiFi but out of charger range no longer looks dead.
- **/api/ota** rejects raw (non-multipart) POSTs with 415 instead of a
  misleading 200.
- **BLE UUID config-mismatch** backs off hard so the web UI stays
  responsive instead of fail-looping.
- Cross-boot event attribution: boot-settle flag + per-boot id so
  early-boot reconnect events stop looking like faults.

### Added

- **One-click Compatibility Report** (Info → Tools): a read-only probe that
  captures the charger's BAPI method support + BLE GATT layout into a
  paste-ready block, so new charger models can be mapped without the
  hardware in hand.
- **r_lse live-session sensors** over MQTT: per-session solar vs grid kWh
  split, surplus power, active feature, control mode (5 discovery entities).
- **/info diagnostics:** heap min-ever watermark + largest free block.
- Optimistic schedule UI with debounced reconcile (faster saves, avoids 429
  bursts).

## [3.0.0] - 2026-06-12

The platform release. Three changes worth a major-version bump:

1. **AsyncWebServer everywhere.** The whole HTTP surface migrated
   from the bundled Arduino `WebServer` to `mathieucarbou/ESPAsyncWebServer@3.6.0`
   over ten staged steps (A–J). The sync server has been fully
   retired in STA mode; AP mode still uses the sync path as a
   bring-up fallback.
2. **WebSocket pushes for live state.** No more 1 Hz HTTP polling
   from the dashboard. The browser receives `r_sta`, `r_dca`, settings
   merges, and BLE health pushes over `ws://host/ws` (attached to the
   same listener as HTTP — no separate TCP port).
3. **First-class HA integration story.** Companion HA Add-on
   (dashboard + OTA upload) and native HA Integration (sensors +
   controls) ship alongside this firmware as their own repos. MQTT
   discovery continues to work unchanged for users who already have a
   broker — the new paths are for everyone else.

### Added

- **Async HTTP layer** built on ESPAsyncWebServer + AsyncTCP.
  `WB_ASYNC_WEB` build flag was the staging switch during migration;
  both paths build clean, async is the default in `feature/3.0`.
- **AsyncWebSocket push channel** at `ws://host/ws`. Frame envelope
  is `{"t":"<type>","d":<payload>}`. Types: `status` (r_sta),
  `meter` (r_dca), `settings` (merged poll), `ble`
  (`{state, rssi, last_activity_s}`).
- **New `/api/command` query parameters** to control sync vs async
  for BAPI calls (`?wait=N`, `?wait=0` for pure async, `?sync=1`
  for legacy blocking behaviour). Pairs with the existing
  `/api/command_status?id=N` poll endpoint from 2.7.0.
- **Power-flow card** on the dashboard. Animated Grid → Charger → EV
  flow with battery indicator and current-direction arrows. Matches
  the Wallbox app's visual language.
- **One-tap pause/play per schedule row.** Toggle a single schedule
  without opening the editor.
- **Hourly Wtime auto-sync.** Gateway pushes the charger's clock from
  its own NTP source every hour, so the charger never drifts more
  than ~60 minutes from real time.
- **Proactive BLE pairing on connect.** Reduces the "first command
  fails on cold connect" window.
- **First-time setup wizard at `/setup`.** A 4-step linear flow
  (WiFi -> MQTT -> Charger BLE -> Web Security) replaces the one
  long form for first-time onboarding. Each step has Skip,
  pre-fills from current config when re-running, and ends in a
  single submit to the existing `/save` handler. AP-mode root
  redirects here so captive-portal users land in the guided flow.
- **Tabbed `/config` and `/info` pages.** Same idiom as `/settings`
  — five tabs on `/config` (WiFi / MQTT / Charger BLE / Security /
  Advanced), three on `/info` (Overview / Diagnostics / Tools).
  Active tab persists in localStorage. Long single-page scrolling
  replaced with focused sections.
- **WiFi-join failure feedback.** When a save+reboot lands a wrong
  password (or hits a 5 GHz-only / WPA3-only router), the gateway
  records the disconnect reason to NVS and the setup wizard shows
  a banner on the next AP-mode boot explaining what likely went
  wrong (auth fail, SSID not found, handshake timeout). Cleared
  on the first successful WiFi join.
- **Schedule-paused indicator + Resume button.** New MQTT
  `binary_sensor.schedule_paused` (entity #57) plus a paired
  `button.resume_schedule` (entity #58), and an amber banner on
  `/dashboard` with an inline Resume button. Backed by `r_dat.gen`
  for the read (0 = armed, non-zero = paused) and `s_cmode` with
  `{"mode":0}` for the write. Independent of charging state: a
  manually started charge while the schedule is paused keeps the
  indicator on, matching the Wallbox app exactly. New
  `/api/command?action=resume` endpoint maps to the same call so
  HA automations + the dashboard share one code path.

### Fixed

- **`/info` and `/sessions` served an empty body (0 bytes).** The two
  largest pages (~34 KB) silently returned `Content-Length: 0`:
  `req->send(code, type, String)` copies the whole body into the
  response's own buffer, so two ~40 KB blocks had to be live at once,
  and the second allocation failed on the fragmented heap. Pages
  ≤ 32 KB were unaffected, which is why it hid until a full page
  sweep. Now served via a pull-based filler (`_sendHtmlPage`) that
  hands the built `String` to a `shared_ptr` and `memcpy`s slices on
  demand — no second copy. Applied to all seven HTML page builders.

### Stability (rc1 → rc8)

The release-candidate series hardened the async foundations under
sustained load. Validated at 68 h continuous uptime on a Pulsar Plus
(1 sub-second MQTT reconnect, 0 WiFi, 0 BLE, 0 main-task stalls);
live charge / pause / resume confirmed to mirror the official app.

- **BAPI response ownership refactor.** Responses move through the
  drain path as `shared_ptr<String>` — no copies between the BLE and
  web/MQTT tasks.
- **Pre-rendered MQTT discovery payloads** into a shared buffer — no
  per-publish reallocation flood on the discovery burst.
- **Cross-task mutex** around the cached status/realtime JSON so the
  web and BLE tasks can't read a half-written buffer.
- **WDT-restore leak fixed** in the OTA sync early-return paths.
- **AsyncTCP task high-water mark** surfaced in `/api/diag/runtime`.

- **Schedule writes** were silently broken since v2.1.0 — the BAPI
  shape diverged from what newer firmware expects. Now uses `s_sch`
  with integer `start`/`stop`, the `par.schedules[]` wrapper, and
  the bit-array `days` field. Schedules created from the dashboard
  round-trip correctly.
- **`s_alo` and `s_ecos` BAPI write shapes** for fw 6.11.x. Auto-lock
  now uses the bare-integer seconds shape; Eco-Smart uses the
  `{ese, esm, esp}` object shape with `esp` preserved across mode
  changes.
- **Charger-timezone conversion** in `saveSch`/`loadSchedules`. The
  old `Date.toLocaleString` round-trip mis-applied the offset on DST
  boundaries; replaced with `Intl.DateTimeFormat.formatToParts()`
  driven `tzOffsetMinutes`/`localToUtc`/`utcToLocal` helpers.
- **`[object Object]` toast** on schedule save failure — the BAPI
  error envelope is a structured object, not a string. `toast()`
  hardened to render the `.message` field with a JSON fallback for
  unknown shapes.
- **`/style.css` and `/app.js` 404s** after the port swap (task #78
  step J). Both routes had been missed in the step C migration —
  hotfix extracted the literals into shared `wb_getStyleCssLiteral()`
  / `wb_getAppJsLiteral()` accessors callable from both async and
  sync handlers.
- **WebSocket port collisions** during migration. The legacy
  links2004 server briefly ran on `:81` (collided with sync HTTP
  after the swap), then `:82`, then migrated to AsyncWebSocket on
  `:80/ws`.
- **Schedule save/delete robustness** — `delSch`'s clear-then-write
  loop now aborts on `clr_sch` errors instead of forging ahead and
  writing into an unknown schedule slot. Edit-mode no longer
  duplicates the slot id.
- **`WB_ASYNC_WEB=0` default broke STA-mode HTTP entirely.** When
  the sync server was retired in STA mode (this same release), the
  default build flag still left the async server compiled out —
  net effect: a fresh-clone build of 3.0 served NO HTTP on STA
  after the first save+reboot. Default flipped to `1`. Anyone
  needing the legacy sync-only build can still pass
  `-D WB_ASYNC_WEB=0` at the CLI.
- **MQTT discovery `object_id` deprecated.** HA Core 2026.4
  removed `object_id` from the MQTT discovery schema in favour of
  `default_entity_id` (which must include the platform prefix).
  All six publish helpers updated. Existing user entities aren't
  renamed on upgrade — `default_entity_id` only sets the default
  at first registration.
- **CSRF mismatch broke first-time captive-portal save.** Persisted
  CSRF tokens are good for STA-mode browser sessions but bricked
  AP-mode setup: a crash-reboot mid-provisioning regenerated the
  token while the user's form kept the old one, silently 403'ing
  the save. CSRF check now skipped when `wifiSSID` is empty
  (= AP-mode setup), which is safe since the AP is WPA2-protected.
- **BLE-scan + WiFi-scan TWDT panic** when called from the async
  server. Both scans block ~5-8 s on the AsyncTCP task; the
  default 5 s task watchdog fired mid-scan, dropped the
  connection, and the browser surfaced `TypeError: Failed to
  fetch`. Both endpoints now extend the watchdog to 15 s for the
  scan window, restored on completion.
- **Dashboard power-flow + sensor stale-cache after BLE drops.**
  Cached values from a prior charging session would keep driving
  the flow animation indefinitely after BLE disconnected, and the
  numeric tiles (Status, kW, A, V, W) would freeze at last-known.
  `P()` polling + WS `ble` push + `updateBleHealth` polling all
  now call a `_clearLive()` helper that resets every BAPI-sourced
  span and wipes the localStorage rehydration sources.

### Changed

- **Sync `WebServer` retired in STA mode.** `beginSTA()` no longer
  starts it; `loop()` only pumps `http.handleClient()` while in AP
  mode (provisioning). Async serves everything else.
- **Schedule load handler** uses the longer 20 s retry timeout on
  the second attempt (up from 15 s on the first), since the BLE
  task can be saturated in the seconds immediately after a write.
- Table-driven HA MQTT discovery moved from the per-call hand-rolled
  topic builders to the topic table introduced in task #77.

### Removed

- `links2004/WebSockets@^2.4.1` dependency — replaced by
  `AsyncWebSocket` inside ESPAsyncWebServer. One fewer TCP listener,
  one fewer library to track for security advisories.

### Compatibility

- ESP32-S3 only. No Arduino-IDE board variant changes.
- Companion repos:
  [`wallbox-gateway-ha-addon`](https://github.com/botts7/wallbox-gateway-ha-addon)
  v0.2.0 and
  [`hass-wallbox-gateway`](https://github.com/botts7/hass-wallbox-gateway)
  v0.2.0 ship as part of this milestone.

## [2.7.0] - 2026-06-06

The main-loop blocker pass. HA automations no longer freeze
anything when they toggle the charger; web requests still preserve
their byte-for-byte sync response shape but no longer starve MQTT
or WebSockets while a BAPI roundtrip is in flight.

Concretely: every HA-driven command (start/stop, current, lock,
schedule, eco/halo/power-boost) used to occupy the main task for
~400-800 ms during the BAPI write. Under HA's burst patterns
(autodiscover, multiple automations firing in sequence) this
compounded into multi-second windows where the UI lagged and the
WebSocket push paused. The 2.7.0 work pushes all of that off the
main task into a dedicated BLE request queue + response map, and
the web handler's wait loop now pumps MQTT + WS in 50 ms ticks.

### Added

- **New `/api/command` query parameters** for callers that want
  explicit control over the sync/async trade-off:
  - `?wait=N` — short-wait deadline in milliseconds, 0..8000,
    default 5000. The web handler waits up to N ms for the BAPI
    response, then either returns 200 + body (synchronous shape)
    or 202 + `{"id":N,"status":"pending"}` (poll via
    `/api/command_status`).
  - `?wait=0` — pure async. Returns 202 immediately with the
    request id; the BLE task processes in the background.
  - `?sync=1` — escape hatch preserving the pre-2.7.0 blocking
    behavior (with a tighter inflight cap of 1 to keep legacy
    callers from doubling up). For any caller that depends on
    the exact byte-for-byte response shape.
- **New `/api/command_status?id=N` endpoint** — poll for a
  previously-enqueued async response:
  - `200` + body — response landed
  - `202` + `{"id":N,"status":"pending"}` — still in flight or
    evicted from the small RAM map but might still be tracked
  - `410 Gone` — `id` is from the future (never issued by this
    gateway boot), retry-discouraged
  - `400` — missing or zero `id`
- **Test harnesses** (`tests/`):
  - `api_command_async.py` — 13 unit tests covering the new
    contract, baseline sync latency, queue plumbing, 410 path.
  - `edge_cases.py` — 13 tests for parallel correlation, URL
    edge cases (negative/huge/garbage `?wait`, oversize `par`),
    token bucket + queue overflow recovery.
  - `longevity.py` — 4 tests for memory stability over 90 s soak,
    WebSocket resilience under HTTP burst, MQTT command storm.
  - `hardening.py` — reusable soak/burst harness with a charger
    monitor thread that polls `r_dat` and validates the EVSE
    stays healthy throughout.
  - `ui_surface.py` — 18 probes mirroring every BAPI fetch the
    web UI's JS makes.

### Changed

- **WiFi reconnect is event-driven, not polled.** The pre-2.7.0
  `checkWiFi()` ran every 30 s on the main loop and called
  `WiFi.reconnect()` synchronously whenever the link was down.
  That sync call could block the main task for several seconds
  during driver scan / auth / DHCP. New module `wb_net` registers
  `WiFi.onEvent()` handlers (on the driver's event task —
  flags-only, no real work) and a `wb_net::tick()` on the main
  loop drains the deferred work: mDNS rebind, diag report
  Disconnect/Reconnect, and the explicit `WiFi.reconnect()` call
  ONLY when the driver's own auto-reconnect has clearly given up
  (≥ 60 s since last GOT_IP) and a 1/2/5/15/60 s backoff window
  has elapsed. Most transient flaps recover via the driver's
  auto-reconnect before our 60 s gate elapses; the explicit
  reconnect never fires. Measured: `loop_max_ms` baseline dropped
  from 1500-2000 ms to ~17 ms post-OTA on the maintainer's MAX.
- **WiFi reconnect counter exposed** at `/info` → Connection
  Diagnostics → Reconnect counters, parallel to BLE and MQTT.
  Also surfaced in `/api/diag/disconnects` for HA dashboards.
  The 30 s loop_max_ms grace window already extended by
  reportReconnect() now covers WiFi too — an explicit
  `WiFi.reconnect()` call won't trigger the tripwire.
- **MQTT command path is fully non-blocking.** `_handleCommand`
  in `wb_mqtt.cpp` now enqueues every BAPI write
  (start_stop, current, lock, autolock, eco/schedule/power_boost/
  halo, native HA entity handlers) on a FreeRTOS queue and
  returns immediately. The raw `wallbox/bapi` passthrough uses
  `MQTT_PUBLISH` reply mode so its response still lands on
  `wallbox/response/<met>` for any existing subscribers. No
  observable behavior change to HA — just no main-loop occupancy.
- **Web request handler pumps MQTT + WebSocket every 50 ms** while
  waiting for a BAPI response. Pre-9c, a 5 s `?wait` would freeze
  the WebSocketsServer's accept loop (browsers saw
  `ERR_CONNECTION_RESET` on the first WS handshake every page
  load) and pause PubSubClient. The chunked pump keeps both
  responsive throughout the wait. Re-entrancy verified safe: MQTT
  callbacks post-Step-8 only enqueue, never re-enter
  `sendCommand`; the WebServer is never re-entered (we're already
  inside `handleClient()`).
- **`loop_max_ms` warning thresholds** in /info → Connection
  Diagnostics now reflect 2.7.0 reality. ≤ 2000 ms is default
  color (normal operation), 2000-8000 ms is amber (wait path
  exercised), > 8000 ms is red + ⚠ (above clamp — genuinely
  abnormal). Also fixes a left-to-right ternary bug that made the
  red branch unreachable.

### Fixed

- **`_nextReqId` race condition** (HIGH, caught by pre-release
  audit). `enqueueRequest` runs from both the main task (web
  handler) AND the BLE task (MQTT callback fires inside
  `sendCommand`'s `onYield`). Plain `_nextReqId++` would race
  and issue duplicate ids, corrupting the response map. Replaced
  with `__atomic_fetch_add(&_nextReqId, 1, __ATOMIC_RELAXED)`;
  loops on the result to skip the 0 sentinel under wrap-around.
- **`?wait=0` BAPI early-timeout regression** (caught by new
  parallel-correlation tests). Step 9d computed
  `bapiTimeout = waitMs + 250`, so pure-async callers gave BAPI
  only 250 ms to respond — under any real charger roundtrip.
  Empty responses were filtered out by `_storeResponse`, so
  `/api/command_status` polled forever. Fixed: `bapiTimeout = 0`
  (BLE-task default 5 s) for the `waitMs == 0` case.
- **WS dropping under HTTP load** (visible symptom of the missing
  chunked pump). Fixed by step 9c above.

### Known constraints

- The web `/api/command` path can still occupy the main task for
  up to `?wait` (default 5 s) on a slow BAPI call. The chunked
  pump keeps MQTT and WS responsive during that window, but the
  main loop's own iteration time grows by the same amount.
  Truly non-blocking web would require migrating to
  ESPAsyncWebServer — already on the 3.x roadmap.
- The 8 s `?wait` upper clamp means BAPI calls that legitimately
  take > 8 s (rare; only `gupdc` cloud check observed in this
  range) will return 202 even with `?wait=12000`. The JS clients
  fall back to "Couldn't read X state" rendering, which is the
  same path as a real BAPI error and matches pre-2.7.0 UX.

## [2.6.1] - 2026-06-06

Quick fix for @mvanlijden's [#11](https://github.com/botts7/esp32-wallbox/issues/11):
a Pulsar MAX on FW 6.11.26 looped forever on "Service not found"
because at that firmware Wallbox migrated MAX to the dual-char
protocol that previously only shipped on Plus/Copper/Quasar.
Hardware-correct chargerModel="max" no longer implies the older
single-char BLE protocol on newer firmware.

### Fixed

- **Auto-detect protocol-family mismatch on BLE connect.** When the
  configured-model service UUID isn't found in the GATT topology
  but the OTHER family's service IS present, the gateway now
  adopts the correct protocol in memory, persists the new model
  + UUIDs to NVS, and continues the connect inline. A loud log
  block tells the user what happened. Handles both directions
  (MAX→Plus for FW ≥ 6.11.26, Plus→MAX if someone mis-set the
  dropdown the other way). No disconnect/reconnect cycle — the
  fall-through path picks up the corrected UUIDs immediately.

  Live-verified on the maintainer's MAX by deliberately
  misconfiguring chargerModel to "plus" and watching the gateway
  auto-switch back on the next boot's connect attempt.

## [2.6.0] - 2026-06-03

Architectural fix for the main-loop wedge @peter-mcc reported on 2.5.1:
his `loop_max_ms` metric showed 80 000 ms overnight, root-caused to
the HA-discovery burst hitting a stalled MQTT broker and compounding
~60 sync TCP writes in series.

### Fixed

- **HA discovery is now bounded to one publish per main-loop
  iteration.** `WallboxMQTT::sendDiscovery()` now ARMS a state
  machine and returns immediately; a new `tickDiscovery()` called
  from the main loop publishes one entity per tick. Under a healthy
  broker the burst still completes in ~600 ms wall-clock (the
  existing `delay(10)` per loop iter × 57 ticks); under a wedged
  broker the per-loop cost is now bounded to **one socket timeout**
  (~1 s) instead of compounding to tens of seconds.
- **MQTT socket timeout dropped from 5 s default to 1 s.** Combined
  with one-per-tick, worst-case `loop_max_ms` during a broker outage
  is now ~1100 ms instead of 80 000 ms. Marginal links may see a
  publish fail that would previously have succeeded; both discovery
  (idempotent retained) and state publishes (next BLE-cache advance
  re-publishes) self-heal.

### Changed

- **HA Device page reorganised**: ~12 debug entities (Loop Max ms,
  Heap Free, Reentry Tripwire, etc.) now carry
  `entity_category: diagnostic` so HA collapses them into a separate
  section instead of mixing with user-facing sensors. peter-mcc
  2.5.1 feedback on metric clutter.
- **Dropped "Status Code (raw)" sensor** (was exposing
  `r_sta.charger_status` as a raw int with an undocumented enum).
  The friendly "Charger Status" sensor from PR #7 is the canonical
  user-facing value. Migration: `sendDiscovery()` publishes an empty
  retained payload to the old discovery topic so existing HA
  installs delete the stale entity.

### Live-validation on the maintainer's Pulsar MAX

- `loop_max_ms` post-burst: **29 ms** (was 80 000 ms on 2.5.1
  worst-case, ~500 ms typical)
- All 56 entities populate in HA within ~1 s under a healthy broker
- `Charger Firmware` flips from gateway-fw fallback to charger app
  FW (6.11.16) within ~1 s of BLE init completing — confirms the
  state machine re-arms correctly on `discoveryStale`
- Toggle paths (Auto Lock, Charging, Lock) round-trip cleanly
- Diagnostic-category entities render in their own HA section

### Architecture note

This is the third "move blocking work off the main loop" change on
this branch. rc16 moved BLE polling onto a FreeRTOS task. 2.4.3
gated `loop_max_ms` during reconnect windows. 2.6.0 lifts the HA
discovery burst out of the main loop's hot path. Remaining
main-loop blockers identified by the 2.5.1 audit and deferred:

- `WiFi.reconnect()` blocking 15-30 s on a stuck AP
- `/api/command` BLE-passthrough blocking up to 5 s

These are smaller and more targeted; queued for future releases if
real-world wedges keep landing on them.

## [2.5.1] - 2026-06-02

Root-cause fix following @peter-mcc's 2.5.0 feedback. He reported
needing to press "Clear counters" three times after upgrading before
it took effect.

### Fixed

- **CSRF token now persists in NVS across reboots.** Previously the
  token regenerated each boot (random-seeded at first `ensureCsrfToken()`
  call), so a `/info` page held open across an OTA still held the OLD
  token in `window.WB_CSRF` — every state-changing fetch from that
  stale page silently 403'd with no UI feedback. Looked like the
  button was broken. Persisting the token means the browser's cached
  copy stays valid post-reboot. No client-side recovery dance needed,
  no toast, no auto-reload — the stale-token problem can't happen.
  Factory reset explicitly wipes the `wbcsrf` NVS namespace so a
  reset device generates a fresh token instead of inheriting the
  previous installation's.
- Tight-race guard on first-boot token generation: two concurrent
  `ensureCsrfToken()` calls during the brief window before NVS holds
  a token could previously generate different tokens and leave the
  RAM copy out of sync with the persisted one. Now gated by a single
  `csrfTokenReady` flag.

### Threat-model note

CSRF tokens are double-submit tokens, not session secrets. A
long-lived per-device random 128-bit hex value is fine — it's only
ever readable from an already-authenticated session (it's embedded
in pages rendered behind `checkAuth()`). The previous boot-rotation
gave no real security benefit and caused the UX bug above.

## [2.5.0] - 2026-06-02

Three community PRs from **@benvanmierloo** that fix real HA-side
behaviour bugs, plus follow-ups for findings @peter-mcc surfaced on
2.4.3. All verified live on the maintainer's Pulsar MAX.

### 🙏 Community contributions

- **Decode local BLE status enum instead of cloud codes**
  ([#7](https://github.com/botts7/esp32-wallbox/pull/7) by
  @benvanmierloo). The 2.4.x status table was a mix of cloud
  `status_id` codes (`161`/`178-180`/`189-194`/`209-210`/…) and a
  partial local-enum set. The local BLE protocol (`r_dat.st`)
  actually returns a clean 0-18 enum on both MAX and Plus per
  jagheterfredrik/wallbox-ble. The two clash at several codes,
  worst at `st=6`: locally it means LOCKED, in the cloud table it
  meant ERROR. A locked charger was rendering as "Error" + "car
  unplugged" in HA. This release adopts the local 0-18 enum
  everywhere — Charger Status sensor, dashboard, notification
  triggers, car-connected logic. The maintainer's MAX with Eco
  Smart enabled now correctly reads "Queued (Eco-Smart)" (st=18)
  instead of the previous "Waiting for Schedule".

- **Render HA charging/lock as real toggles, not assumed-state
  buttons** ([#8](https://github.com/botts7/esp32-wallbox/pull/8)
  by @benvanmierloo). The Charging and Charger Lock MQTT switches
  rendered as a pair of on/off buttons because HA's state never
  resolved — `payload_on`/`payload_off` were the command strings
  (`start`/`stop`, `lock`/`unlock`), but the value_template emits
  `"1"`/`"0"` which matched neither. Added explicit `state_on`/
  `state_off` to discovery so HA matches the template output and
  renders proper sliding toggles.

- **Auto Lock controls actually work**
  ([#9](https://github.com/botts7/esp32-wallbox/pull/9) by
  @benvanmierloo). Three connected fixes:
    - `g_alo` returns the timeout as a bare scalar (`{"r":60}`,
      `0` = off). The 2.4.x poll mis-parsed it as an object, so
      the Auto Lock switch stuck OFF and the timeout always
      showed a default.
    - `s_alo` also expects a bare scalar — HA was sending an
      object, so writes were silently ignored. Now sends the
      scalar; toggling ON restores the last-seen timeout
      instead of resetting to a default.
    - The HA Auto Lock Timeout is now expressed in **minutes**
      (1-60) to match the Wallbox app, converted to seconds at
      the BAPI boundary. Rendered as an exact-value input box
      instead of a 60-step slider.

  Manual port to the BLE-task architecture (the PR was written
  against the rc15-era main.cpp polling — periodic settings
  reads now live on `_pollSettings()` inside the BLE FreeRTOS
  task).

### Added

- **Persistent boot record in OTA history**. `wb_health::markHealthy()`
  now writes a `kind: "boot"` entry to the OTA-history NVS ring,
  capturing the version that successfully reached healthy state.
  Pairs with the existing `kind: "ota"` upload events (written by
  the *previous* firmware at upload time) to give `/info` a complete
  chronological "what got installed AND which versions actually
  booted" story. The renderer shows boot entries with a blue arrow,
  upload events with the existing green/red dot. @peter-mcc 2.4.3
  follow-up: he reported the OTA history was "missing 2.4.2" — the
  upload event recorded by the old firmware was always there, but
  there was no record confirming 2.4.2 successfully booted; the new
  half closes that gap.

### Changed

- **OTA history capacity bumped 5 → 20.** Same NVS-backed ring,
  same write semantics. The 5-entry limit rolled past Peter's
  2.4.2 entry after a couple of subsequent OTAs; 20 entries cover
  a typical month of upgrade activity without bloating NVS
  (~2 KB max).
- **"Clear counters" button on `/info` also resets `loop_max_ms`.**
  Previously the tripwire stuck at its boot-time max until reboot,
  leaving no recourse for a one-off outlier. The button now zeroes
  both `g_loopMaxMs` and the in-flight loop-gate deadline alongside
  the BLE/MQTT reconnect counters.

### Fixed

- **HA discovery device block consolidated** under
  `populateDeviceBlock()` for all 6 helpers (sensor / switch /
  number / button / select / car-connected binary_sensor). Before
  2.4.2 the helpers had drifted — only the sensor one carried
  `sw_version`, others were missing `connections`. HA's
  merge-by-identifiers semantics meant the device-page metadata
  flickered based on payload arrival order. 2.4.2 already
  consolidated; 2.5.0 re-verifies after the PR #7 / #8 / #9
  integration that every entity type still emits the same complete
  block.

### Eliminated, then re-verified as charger behaviour

An "optimistic publish" path was briefly added then **reverted**
during 2.5.0 development. Live testing revealed that the visible
toggle "bounce" was the Wallbox charger acting unilaterally — it
auto-releases the socket lock after ~7 s when no active session
needs it, and it keeps st=18 (Queue Eco-Smart) when Eco-Smart is
gating charging regardless of how many manual Start commands you
fire. Painting over those state transitions optimistically would
mean lying to the user about what the charger is actually doing.
The repoll path that confirms reality within ~500 ms after a
write was kept — bounce window collapsed to fast accurate feedback.

### Migration

If you're upgrading from 2.4.3 via OTA, your existing OTA history
ring will retain its older entries (the new code reads them
backwards-compatibly through the `from` field). New entries will
use the richer `kind`/`version` schema. No action required.

## [2.4.3] - 2026-06-02

Tightens the `loop_max_ms` tripwire so it stops false-positiving on
sync MQTT/BLE reconnect blocking. Surfaced by @peter-mcc shortly after
2.4.2 shipped.

### Fixed

- **`loop_max_ms` saturated by MQTT reconnect blocking.** When the
  MQTT broker briefly goes away — an HA add-on reload, a router
  hiccup — sync `PubSubClient::connect()` blocks the gateway's main
  task for ~15 s per attempt while it retries. The tripwire
  faithfully measured that as a 30-40 s loop gap, but it wasn't the
  kind of *unprovoked* runtime wedge the metric was built to catch.
  Once the value saturated, any real wedge underneath was invisible.
- New `wb_diag::extendLoopMaxGate(graceMs)` API. Called automatically
  from `reportReconnect()` (the existing single-source-of-truth for
  tracked reconnect events) — every successful BLE or MQTT reconnect
  pushes a 30 s grace window forward. The main loop's gap tracker
  consults `loopMaxGateActive(now)` and skips recording during that
  window.
- Overlapping reconnects only extend the window, never shorten it.
  The gate auto-clears on expiry so the next event re-arms cleanly.

### Layered filters now stacked on `loop_max_ms`

What lands in the metric after **all three** filters fire is a
genuine unprovoked wedge worth investigating:

1. First 60 s of uptime (boot-phase MQTT discovery flood — 2.4.1)
2. 30 s after any tracked BLE/MQTT reconnect (this release)
3. The trivial "first-iteration timestamp is meaningless" check

### Live-validation

Confirmed on the maintainer's MAX by stopping and restarting the
Mosquitto broker:

  Before: loop_max_ms = 31 ms, mqtt_reconnects = 0
  After:  loop_max_ms = 265 ms, mqtt_reconnects = 1, longest 10 s

Without the gate that 10 s outage would have produced 10 000+ ms.
With the gate it surfaces as 265 ms — comfortably under the 500 ms
yellow threshold. Heap, BLE, and 2.4.2 fields all intact through and
after the test.

## [2.4.2] - 2026-06-01

Follow-up release driven by @peter-mcc's testing of 2.4.1 on his
Pulsar Plus. Two real bugs and a quiet drift between the discovery
helpers — all fixed live on the maintainer's MAX before tagging.

### Fixed

- **Total Charging Sessions sensor was reading the wrong field.** 2.4.1
  surfaced `r_ses.size` which is actually the log-buffer capacity
  sentinel (99 999 on MAX, missing on Plus) — not a lifetime count.
  Switched to `r_ses.last` (verified 233 on the maintainer's MAX);
  kept `size` as a fallback but only when in a plausible range so the
  sentinel can't sneak in. When neither field yields a real value
  (some Plus firmwares), `chg_sessions` publishes JSON `null` so the
  HA sensor goes unavailable instead of sticking at 0 or -1 forever.
- **HA Device-page firmware label was hardcoded.** 2.4.1 wrote
  `dev["sw_version"] = "6.11.16"` in the discovery device block, so
  every Pulsar — Plus, Quasar, Copper, anything that isn't a MAX —
  showed `6.11.16` in HA even though the per-entity Charger Firmware
  sensor was correct. Now driven by `wallboxBLE.chargerAppFirmware()`
  with a `WB_VERSION` fallback until `fw_v_` has been read. BLE init
  raises a one-shot `_discoveryStale` flag the first time the value
  populates, and the main loop triggers `wallboxMQTT.sendDiscovery()`
  once to update HA in place.

### Changed

- **Single source of truth for the HA discovery device block.** Six
  discovery helpers (entity / switch / number / button / select / the
  inline car-connected binary_sensor) each built the device block
  inline and had drifted — only the entity helper carried `sw_version`,
  the others didn't. Consolidated into `populateDeviceBlock()`. Every
  payload now carries the same complete block:
    - `identifiers`, `name`, `manufacturer`, `model`, `sw_version`
    - `connections: [["mac", <wifimac>]]` so the HA Device page header
      shows the gateway's MAC alongside the firmware label
    - `configuration_url: http://<gw-ip>/` so the HA Device page header
      gets a "Visit" button deep-linking to the gateway dashboard

### Migration notes

If you're upgrading from 2.4.1 and your HA Device page still says
"Connected via Unnamed device" or shows stale entities (the
`chg_net_rssi` sensor from the dev build), the cleanest reset is to
delete the Wallbox device in HA (Settings → Devices → device → Delete)
and reboot the gateway via `/info` → Reboot Gateway. The fresh
discovery payloads will rebuild the device with everything correct;
HA stops linking it under the MQTT broker hub once the device block
carries a full `connections` array.

## [2.4.1] - 2026-05-31

### Added

- **Charger firmware + project identity.** New `fw_v_` BAPI read at BLE init
  pulls the charger's app firmware and project string (e.g. `prj15-pulsar-max`).
  Surfaced on `/info` (new "Firmware" section under Charger Details) and as a
  separate **Charger Firmware** entity in Home Assistant — previously only the
  BLE module's firmware was visible, which routinely got mistaken for the
  charger's app version.
- **Model auto-detection.** `inferredModel()` maps the charger's project
  string to a friendly name (Pulsar MAX / Plus / Copper SB / Quasar / Quasar 2).
- **Total charging sessions counter.** Reads `r_ses` once per BLE connection,
  exposes as `chg_sessions` on `/api/status`, ships a `total_increasing`
  sensor to HA, and renders as a badge next to the Charging Sessions page
  header.
- **Power Boost (ICP) limit.** `r_hsh` BAPI read; surfaced as `chg_power_boost`
  and an HA sensor with `A` unit.
- **Discrete lock state.** `r_lck` returns 0=unlocked / 1=locked; published as
  a `binary_sensor` with `device_class: lock`.
- **Charger-side network detail.** `gnsta` BAPI read; the charger's own
  WiFi SSID / IP / RSSI are now exposed alongside the gateway's network
  in a "Charger Network" section on `/info`.
- **Gateway WiFi panel.** `/info` Charger Details card now also shows the
  ESP32's own SSID / IP / RSSI in a "Gateway WiFi" section.
- **Loop-wedge tripwire (`loop_max_ms`).** Tracks the longest gap between
  consecutive main `loop()` iterations since boot. Healthy values are under
  ~200 ms in steady state; multi-second values indicate a wedge of the kind
  observed during long HTTP responses in earlier RCs. Surfaced on
  `/api/health`, MQTT, and the new "Runtime Health" card in `/info`'s
  Diagnostics view.

### Changed — OTA hardening

- **503 + Retry-After on admission rejection.** The OTA upload path no
  longer responds with a generic 500 when admission denies an upload — it
  emits a proper `503` with `Retry-After` indicating when to retry (the
  exact uptime crossover, plus a small cushion).
- **Browser auto-retry on 503.** `/ota` page's upload script parses
  `Retry-After`, runs a live countdown, and automatically re-fires the
  upload once. Eliminates the manual re-click that previously followed
  every just-after-reboot OTA attempt.
- **Relaxed admission window after first successful OTA.** A device that
  has completed one OTA + reached healthy state on the new firmware
  flips a NVS-backed `ota_proven` flag. Subsequent admission checks drop
  the minimum uptime threshold from 60 s to 15 s.
- **RAII watchdog scope (`wb_watchdog` module).** Backport of the
  ESPHome PR #16138 pattern. `wb_wdt::extendTo(60)` / `wb_wdt::restore()`
  guarantee the Task WDT is restored to its default 5 s timeout even
  when the OTA path early-returns on error — previously a failed OTA
  could leave the WDT permanently relaxed.
- **Optional MD5 verification.** When clients (curl, HA OTA scripts) send
  an `X-Firmware-MD5` header, the Update library streams the hash
  alongside the partition writes and refuses to commit on mismatch.
  Browser uploads continue to skip this to avoid the multi-second
  hash step on 1.5 MB files.
- **`ota_proven` and `ota_min_uptime` exposed on `/api/health`** so
  testers can confirm the relaxed admission window engaged without
  scraping logs.

### Documentation

- **`RELEASING.md`** documents the four pre-release gates introduced
  after the rc23/24/25 cycle: fresh-install smoke, browser OTA, stress +
  tripwires, and read-only BAPI probe. A tagged non-RC release must pass
  all four.

### Background / Why

A run of release candidates (rc23 → rc25) shipped fixes that passed the
maintainer's curl-based tests but failed when @peter-mcc actually exercised
them in a browser. 2.4.1 closes the surface gap: every new diagnostic,
every OTA path, and every charger-side exposure was validated end-to-end on
a live Pulsar MAX before tagging.

## [2.3.0] - 2026-05-22

### 🙏 Community contributions

- **BLE SMP pairing for Wallbox firmware ≥ 6.11.26** — newer charger firmware
  requires an encrypted BLE link before allowing CCCD writes (notification
  subscription). The gateway now tries the plain `registerForNotify` first
  (backwards-compatible with older firmware) and falls back to SMP passkey
  pairing using the configured BAPI PIN if rejected. Tested on FW 6.11.16 (no
  fallback needed) — anyone on 6.11.26+ should now be able to use the gateway.
  Thanks to **[@benvanmierloo](https://github.com/benvanmierloo)** for the
  contribution! ([#1](https://github.com/botts7/esp32-wallbox/pull/1))
- **Telnet log server on port 23** — all `Serial` output is now also streamed
  to up to two LAN telnet clients (`telnet wallbox-gw.local`). Service is
  advertised via mDNS. Zero overhead when no client is connected. Also by
  **[@benvanmierloo](https://github.com/benvanmierloo)**.

### Added

- **Solar savings** on the `/sessions` stats tiles. Week / Month cost tiles now
  show an additional ☀ "saved $X" line — the dollar value of solar (green) kWh
  vs what grid would have cost at your tariff. Configurable green rate (default
  $0 = "free" solar; set to your feed-in tariff to see opportunity cost).
- **Notifications panel** (Settings → Charger → 🔔 Notifications) — detects
  that the gateway runs on plain HTTP (no Notification API access) and shows a
  ready-to-paste Home Assistant automation snippet that gives proper push
  notifications via the HA Companion app, using our existing
  `sensor.wallbox_pulsar_max_status` entity.

### Fixed

- **Page reveal** now triggers on `DOMContentLoaded` instead of `window.load` —
  much faster perceived load when the gateway is busy talking to a weak BLE
  link (was waiting on every CSS/JS asset before showing anything).

## [2.2.0] - 2026-05-19

### Added
- **Cost tracking** with time-of-use tariffs. Local tariff editor in Settings → Charger → 💰 Charging Cost. Configure base $/kWh, optional solar (green) rate, and any number of TOU periods (name, rate, day-of-week chips, from/to hours). Top-down match; first tier wins. Stored in `localStorage`. Costs displayed on `/sessions` as Week/Month tiles, $ per day, $ per session (in expanded view), and a new Cost column in CSV export.
- **WebSocket live dashboard** on `:81/`. Server pushes `status` / `meter` / `settings` / `ble` updates as they happen — no polling. Dashboard tiles update in real time. Falls back to HTTP polling automatically.
- **Cache-first rendering** — dashboard tiles paint last-known values from `localStorage` immediately, before any network activity.
- **Charger notifications** surfaced on dashboard — red bell tile + click-through modal with timestamps.
- **BLE health banner** with five tiers (disconnected / very-weak / unresponsive / weak / struggling) using both RSSI and "seconds since last reply".
- New `/api/status` field: `ble_last_activity_s`.

### Fixed
- **All 6 settings panels** (Auto Lock, OCPP, Eco Smart, Timezone, Phase Switch, Halo LED) refuse to render their form if they couldn't read the current state from the charger. Previously fell back to default values that could silently overwrite real config when Saved.
- **Schedule list** distinguishes "empty" (charger has none) from "couldn't load" (BLE blip → Retry button). Was showing "No schedules. Tap + Add New." in both cases.
- **Dashboard `undefined` / `NaN` tiles** — Status / Charging Power / Charging Current / Session Energy / Max Current would render garbage when a field was missing in the WS push payload. Root cause: handler wasn't unwrapping the `{id, r:{…}}` BAPI envelope. Fix: unwrap + per-field numeric guards. If a value isn't a number, keep the last-known value instead of overwriting with junk.
- BLE health/notification polling skips when BLE state is not `connected` — stops the per-minute hammer on a dead link.

## [2.1.2] - 2026-05-19

### Fixed
- Schedule times were ±1 hour off when the user's timezone DST status differed from the hardcoded `Jan 1, 2024` reference date used in `utcToLocal` / `localToUtc`. Both conversion functions now use today's date so the offset is correct year-round (matters most for southern-hemisphere users after their DST ends in April).

## [2.1.1] - 2026-04-26

### Added
- Light / Dark / Auto **theme toggle** (Settings → Charger → 🎨 Theme). Auto follows the OS preference; manual choice persists in localStorage.
- **CSV export** of cached session history (Sessions page → 📥 Export CSV).
- **Clickable heatmap cells** — tap a cell to see every session that delivered energy in that day×hour.
- **Backdrop-blur** on the drilldown modal.

## [2.1.0] - 2026-04-26

### Added
- **/sessions page redesign**: stats tiles (All-time, This Week, This Month) at top, with localStorage-cached session log so revisits are instant.
- **Daily Charging** view groups sessions by day with totals. Click any day to expand and see every individual sub-session (useful for solar/eco-smart charging that pauses/restarts many times).
- **Load older sessions** button paginates through the charger's full history (delta-fetches only new sids on revisit).
- **Schedule CRUD**: per-row Edit and Delete buttons, "+ Add New" form with auto-assigned sid, list refreshes after each operation.
- **Phase Switch panel**: Settings → Settings tab now opens an editable panel (was read-only).
- **Halo LED panel**: Settings → Charger tab — Standby on/off, brightness slider, standby timeout. Matches the official app.
- **Eco Smart panel** now fetches and pre-fills current state (mode + solar power target) and saves both.
- **Release BLE for App** button: temporarily disconnects the gateway from the charger so the official Wallbox app can BLE-connect. Live amber countdown banner with 5-min default. Auto-resumes.
- **Schedule list shows individual sids** and renders edit/delete icons per schedule.
- **Session start–end times** displayed in CHARGER_TZ (no more browser-local drift).

### Fixed
- **FOUC** (flash of unstyled content): body now hidden until `window.load`. Inline html/body background prevents white flash.
- **Heatmap distributes kWh across hours** instead of dumping the whole session into the start hour. 5-min granularity.
- **Heatmap mobile overflow**: now scrolls horizontally inside its card.
- **Heatmap "stuck at 30/30"**: `CHARGER_TZ` was undefined on the `/sessions` page, causing `buildHeatmap` to throw before writing the session list. Now defined on every page that needs it.
- **Session list sorted by timestamp desc** so today's sessions appear at the top regardless of fetch order.
- **Session timestamps render in CHARGER_TZ** (was browser-local), so all browsers see the same time matching the charger's clock.
- **Eco Smart mode mapping** corrected: `esm:1` is **Full Green**, `esm:2` is **Solar + Grid** (was inverted everywhere — web UI, F() display, MQTT discovery template, and HA select command handler).
- **Day bitmask labels** in schedule list: was using Sun-first array but bitmask is Mon = bit 0. Fixed.
- **Session energy unit confusion**: live `r_sta.en` is in 10-Wh (centi-kWh, divide by 100), but historical `r_log.en` is in Wh (divide by 1000). Both now correct.
- **Schedule TZ race condition**: `Q('r_schs')` now awaits `tzReady` before rendering so times never flash as UTC.
- **External CSS in `<head>` instead of bottom of body** — significantly faster page navigation (parallel fetch with HTML parse).

### Notes
- Discharge energy `den` divisor unverified; only matters for Quasar 2 V2H owners.
- BAPI `r_sta.en/gen/grid` are in 10-Wh units; `r_log.en` is in Wh; `r_dca.e` is in Wh. Different scales for different methods.

## [2.0.1] - 2026-04-18

### Fixed
- Charging current was displayed in raw deciamps (98A showed when actually 9.8A). All currents now divided by 10.
- Session/green/grid energies were displayed in raw watt-hours. Now divided by 1000 for kWh.
- Session Energy tile showed 0 because it was reading `den` (V2H discharge) instead of `en` (session energy).
- WiFi scan result dropdown didn't populate — replaced with visible clickable result list.
- SSID was a locked `<select>` — now a text input with scan suggestions, users can type manually.
- Nav bar items had uneven widths — now use `flex:1 1 0` for equal distribution.
- Button sizes were inconsistent (outline buttons were 2px narrower due to borders). Fixed with `box-sizing:border-box`.
- Phone Chrome aggressive caching — disabled cache headers on CSS/JS, added version query string, service worker purges cache on install.
- Multiple null pointer errors in save functions — replaced result-div writes with toast notifications.
- `sessionEnergy` → `(value_json.r.en / 1000)` in MQTT template.
- `current_lX` HA templates divided by 10.

### Added
- Gateway IP sensor in HA (useful when DHCP lease changes).
- `House Power` / `House Current` HA entities (renamed from Grid Power/Meter Current for clarity).
- `Grid Energy`, `Green Energy`, `Discharge Energy (V2H)` session HA entities.
- `Lifetime Energy` from MID meter for HA Energy Dashboard.
- Time-of-use cost tracking documentation in HA docs (2-tier and 4-tier examples).
- mDNS retry logic (3 attempts) and TXT records for better discovery.
- Clear labels: "Charging Power", "Charging Current", "Session Energy" vs "House Power".

### Changed
- Dashboard tile values rounded to 2 decimals (was showing 2.3520000001 kW).

## [2.0.0] - 2026-04-17

### Added
- **Phase A:**
  - HA native entities for all charger settings (switches, numbers, selects)
  - Auto Lock, Eco Smart Mode, Power Sharing, Phase Switch, Halo LED, Timezone
  - `pollSettings()` merges all config reads into `wallbox/settings` topic
  - Command handlers for all new HA entities (convert HA payloads → BAPI JSON)
  - Slider auto-sends with 300ms debounce (removed "Set" button)
  - Session history browser (last 20 sessions via `r_log`)
  - Factory reset uses toast-style modal instead of browser `confirm()`
  - Weekly session heatmap page (`/sessions`) — day × hour grid, kWh intensity
  - Charger Details card on Info page (manufacturer, model, BLE firmware from GATT)
  - HA diagnostic sensors: charger_name, manufacturer, model, BLE firmware
  - Responsive dashboard tiles (`repeat(auto-fit, minmax(140px, 1fr))`)
  - PWA manifest with inline SVG icon — installable on iOS/Android
  - Minimal service worker
  - Mobile tab scroll fade
  - Home Assistant integration docs (`docs/HOME_ASSISTANT.md`)

- **Phase B (Security):**
  - Web authentication (optional, configurable in NVS)
  - Rate limiting: 1s delay per failed login, 30s lockout after 5 attempts
  - CSRF token protection on POST endpoints
  - Custom 404 page with dark theme
  - `ble_monitor.py` refactored to `WallboxMonitor` importable class

- **Phase C:**
  - Weekly sessions heatmap page

### Changed
- Settings page reorganized into 4 tabs: Schedules, Power, Security, Charger
- Single-panel UI: all editors render into one result area per tab (no duplicate cards)

## [1.0.0] - 2026-04-16

### Initial Release

- **BLE Gateway**: ESP32-S3 with NimBLE, connects to Wallbox BAPI protocol
- **WiFi coexistence**: `updateConnParams`, ping keepalive, smart scan cache
- **MQTT**: Home Assistant auto-discovery with 20+ entities
- **Web UI**: 4-page navigation (Dashboard, Settings, Config, Info)
  - Dashboard: live status, controls (start/stop/lock), current slider
  - Settings: schedules, eco smart, OCPP, auto lock
  - Config: WiFi, MQTT, BLE, web auth, advanced UUIDs
  - Info: gateway stats, raw BAPI tool
- **OTA**: dual partition table with automatic rollback, web upload page
- **Configuration**: NVS persistence, AP captive portal for first-boot setup
- **mDNS**: `wallbox-gw.local` hostname
- **MIT License** with Wallbox trademark disclaimer
- **Confirmed working**: Wallbox Pulsar MAX with u-blox NINA-B22 BLE radio

[2.3.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.3.0
[2.2.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.2.0
[2.1.2]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.2
[2.1.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.1
[2.1.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.1.0
[2.0.1]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.1
[2.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v2.0.0
[1.0.0]: https://github.com/botts7/esp32-wallbox/releases/tag/v1.0.0
