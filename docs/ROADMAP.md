# Roadmap

Living document. Items are grouped by **release target** and **risk class**.
Tick boxes as work lands. Maintainer cherry-picks the next item based on
whatever is biting hardest in the field.

Last updated: see `git log` on this file.

---

## 🔴 Known main-loop blockers — 2.7.0 target

These are residual blocking calls on the main task that we know about
and have deferred. Each one can wedge `loop_max_ms` for multi-second
periods under the right conditions. Same architectural class as the
MQTT discovery burst we fixed in 2.6.0.

- [ ] **WiFi.reconnect off main loop** (task #70 —
  [full plan](plans/2.7.0-wifi-reconnect.md))

  Event-driven (`WiFi.onEvent`) primary + exponential backoff defer
  policy. Event handler sets flags only; `wb_net::tick()` on main
  drives the explicit `WiFi.reconnect()` only when driver's own
  auto-reconnect has clearly given up (≥60 s since `GOT_IP`), with
  1/2/5/15/60 s backoff. Most of the time the driver auto-reconnects
  on its own and we never explicitly call it. New `wb_net` module
  (~150 LOC); `wb_diag` extended for WIFI Kind (auto-gives WiFi the
  same 30 s loop-gate as BLE/MQTT). ~+200 net LOC across 7 files,
  9-step impl order with small/medium/large tags.

- [ ] **`/api/command` BLE-passthrough async** (task #71 —
  [full plan](plans/2.7.0-api-command-async.md))

  Hybrid queue + short-wait + 202-fallback design. HTTP handler
  enqueues to BLE task and waits ~800 ms; if BAPI completes
  (~98 % case on healthy MAX), returns 200 + body byte-for-byte
  same as today. Otherwise returns 202 + `{id, status:"pending"}`;
  response lands on MQTT + WS; `GET /api/command_status?id=N`
  polls. **Bigger win: same treatment applies to MQTT
  `_handleCommand`** (every HA toggle goes through it). New
  `BleReq` queue (depth 6), reqId correlation via existing BAPI
  `id` field. Backward-compat via `?wait=ms` knob + `?sync=1`
  escape hatch. ~+460 LOC across 5 files, 11-step impl order.

---

## 🟡 Reactive follow-ups — 2.6.x point releases

These are queued for "when a tester reports them." We don't need to
fix proactively but we should be ready when the report lands.

- [ ] **SMP passkey path on Pulsar Plus + FW 6.11.26+** (task #72)
  @benvanmierloo's PR #1 (2.3.0) added the SMP fallback but the
  maintainer hasn't tested on real Plus hardware. @mvanlijden likely to
  hit this next after 2.6.1's auto-switch gets him past the GATT scan.
  May need clarification on the 6-digit-app-code vs 8-digit-BAPI-PIN
  distinction.

- [ ] **Plus `chg_net_signal` field shape validation** (task #73)
  `gnsta` on MAX returns 0-100 quality % in the `signal` field. Plus
  might return RSSI dBm. Our parse has a fallback that detects
  negative values and converts RSSI → quality %, but the path is
  untested on real Plus hardware.

- [ ] **README refresh for 2.4.x → 2.6.x feature surface** (task #76)
  Project root `README.md` is stale on community PRs (status enum,
  switch toggles, autolock), persistent CSRF, one-publish-per-tick
  discovery, protocol auto-detect, Diagnostic-category entities,
  dynamic `sw_version`. Separate commit — not bundled with code work.

---

## 🟢 Polish & observability — 2.8.0 target

Nice-to-have improvements that don't fix observed bugs but raise the
quality bar. Lower priority than the 2.7.0 blockers, but each is small
and self-contained.

- [ ] **Smart tripwire — recent-events ring** (task #74)
  `loop_max_ms` is currently "max ever since clear" — one outlier sticks
  for hours. Track last N events with timestamps so users distinguish
  "one transient overnight" from "recurring spikes." ~50 LOC in wb_diag.

- [ ] **Compress or paginate `/api/settings` response** (task #75)
  Audit flagged 67 KB per response. Strip whitespace + GZIP could
  drop ~75%, paginating by section could drop more. Helps slow WiFi
  clients and reduces HTTP-handler wall-clock time.

---

## 🔵 Architectural backlog — 3.x

These are bigger changes that affect the gateway's distribution or
core architecture. Don't need to do — but they're on the radar.

- [ ] **Table-driven HA discovery refactor** (task #77)
  Replace the 57-case switch in `tickDiscovery` with a `DiscoveryEntry`
  struct table (was an alternative considered for 2.6.0 — the switch
  approach was picked for transcription safety; a follow-up cleanup
  can swap it for the table now that the wire format is locked in).
  No functional change. Lower per-tick overhead and easier to add
  entities. Pure code-cleanup.

- [ ] **ESPHome AsyncWebServer backport** (task #78)
  Replace sync Arduino WebServer with AsyncWebServer (me-no-dev).
  Would eliminate per-request main-loop blocking entirely. Big change.
  Research before committing.

- [ ] **HA Add-on packaging** (task #79)
  Package as a Home Assistant Add-on so users on HAOS / supervised
  installs can manage the ESP32 firmware via HA's UI. Different
  distribution channel from the current Releases page.

---

## How items move

1. **Pending** (unticked box) — in queue. Open task in the tracker.
2. **Active** — work has started. Branch named `feature/<task-slug>` or
   committed directly to `v2.4-pulsar-plus` if it's small.
3. **Shipped** — released. Tick the box, link the release tag in the
   item bullet. Commit the roadmap update.

## Branch hygiene

- Small fixes: commit directly to `v2.4-pulsar-plus` (current release
  branch). Branch protection blocks force-push + deletion, normal pushes
  are fine.
- Larger refactors (>200 LOC or multi-file): branch as
  `feature/<task-slug>`, PR back into `v2.4-pulsar-plus` when ready,
  merge with `--no-ff` to preserve the topic-branch history.
- `main` is dormant — releases happen on `v2.4-pulsar-plus`.
