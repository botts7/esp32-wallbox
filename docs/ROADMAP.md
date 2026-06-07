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

- [x] **WiFi.reconnect off main loop** (task #70 — shipped in 2.7.0;
  [full plan](plans/2.7.0-wifi-reconnect.md))

  Shipped as the `wb_net` module + `wb_diag` WIFI Kind. Event
  handlers on the WiFi driver's event task set flags only; main
  loop's `wb_net::tick()` drains pending work and only calls
  explicit `WiFi.reconnect()` after the driver has clearly given
  up (≥ 60 s gate + 1/2/5/15/60 s backoff). Measured
  loop_max_ms baseline post-OTA: 17 ms. See CHANGELOG.md → 2.7.0
  for the full entry.

- [x] **`/api/command` BLE-passthrough async** (task #71 — shipped
  in 2.7.0;
  [full plan](plans/2.7.0-api-command-async.md))

  Shipped as 11 steps + 4 audit/regression follow-ups (9, 9b-9h).
  Final shape: default `?wait` is 5000 ms (not the planned 800 ms)
  to preserve the byte-for-byte sync JS contract; upper clamp
  is 8000 ms. The async path is preserved for `?wait=0` and
  short-wait callers. MQTT `_handleCommand` is fully fire-and-
  forget — every HA toggle is now zero main-loop time. The web
  handler pumps MQTT + WS in 50 ms ticks during waits so neither
  starves. See CHANGELOG.md → 2.7.0 for the full story.

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

- [x] **README refresh for 2.4.x → 2.8.0 feature surface** (task #76
  — shipped)
  Added "Recent releases" block, refreshed HA integration section
  (diagnostic-category entities, non-blocking commands, dynamic
  sw_version), Admin section (mDNS rebind, persistent CSRF, smart
  tripwire), Compatible Chargers (corrected the 6.11.26 line),
  Project structure (now matches src/ + tests/ + scripts/), and
  added @peter-mcc + @mvanlijden to Contributors.

---

## 🟢 Polish & observability — 2.8.0 target

Nice-to-have improvements that don't fix observed bugs but raise the
quality bar. Lower priority than the 2.7.0 blockers, but each is small
and self-contained.

- [x] **Smart tripwire — recent-events ring** (task #74 — shipped)
  Ring of last 8 long iterations (≥ 1 s) with timestamps. Lives in
  `wb_diag::recordLoopEvent` + ring exposed in
  `/api/diag/disconnects → loop_events`. Renders on /info → Connection
  Diagnostics under "Recent long loop iterations" with the same color
  scale as the latched scalar (neutral / amber / red).

- [x] **Compress /settings page body** (task #75 — shipped)
  Build-time gzip via `scripts/precompress_settings.py` PIO
  pre-script. The big static body (~56 KB raw) becomes a
  PROGMEM byte array; new `/settings/body.gz` endpoint serves
  it with `Content-Encoding: gzip`. `handleSettings` sends a
  small stub that fetches + injects + re-runs scripts.
  Measured: total first-load 68 KB → 28 KB (2.4× wire win).

---

## 🔵 3.0 — the big one

3.0 is a coordinated release bundling three pieces of work. All
decisions settled 2026-06-07. 3.0 does NOT ship until ALL three
are implemented AND the HA Add-on (#79) is end-to-end tested
against the new firmware in a real HAOS VM. No partial 3.0
launches.

Implementation gate: 2.7.0 has been in the field long enough that
the maintainer is confident there's no regression to chase.

- [ ] **Table-driven HA discovery refactor** (task #77 — code
  built dormant on `feature/3.0-table-driven-discovery`;
  [plan](plans/3.0-discovery-table.md))
  Replace the 57-case switch in `tickDiscovery` with a
  `DiscoveryEntry` struct table. Code is already committed,
  compiles clean both with `WB_DISCOVERY_TABLE_DRIVEN=0`
  (default, dormant) and `=1` (active). 3.0 flips the default
  to 1; the legacy switch is removed in the same release.

- [ ] **AsyncWebServer migration** (task #78 — plan +
  decisions: [docs/plans/3.x-async-webserver.md](plans/3.x-async-webserver.md))
  Replace sync Arduino WebServer with mathieucarbou's
  ESPAsyncWebServer (pinned tag). Per-route `#if WB_ASYNC_WEB`
  flag for incremental migration. OTA stays sync on port 81
  during the migration, removed only after async-OTA verifies
  under the hardening + longevity harness. ~40 hr effort.

- [ ] **HA Add-on** (task #79 — plan + decisions:
  [docs/plans/3.x-ha-addon.md](plans/3.x-ha-addon.md))
  New repo `botts7/wallbox-gateway-ha-addon`. Python + Flask
  behind HA Supervisor ingress. Three-phase scope: read-only
  dashboard (v0.1) → OTA upload proxy (v0.2) → polish (v0.3).
  Released in lockstep with the 3.0 firmware. ~30 hr effort.

## 🟣 4.x — speculative

- [ ] **HACS custom integration.** A `wallbox_gateway` custom
  component (Python in HA core, distributed via HACS) that
  exposes the gateway's diagnostics as native HA sensors instead
  of via the MQTT bridge. Different audience to the 3.0 Add-on
  (Docker / venv HA installs, not just HAOS) and a much larger
  Python codebase since it would integrate with HA core
  directly. Could deprecate parts of the MQTT discovery surface
  in a follow-on 4.x release.

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
