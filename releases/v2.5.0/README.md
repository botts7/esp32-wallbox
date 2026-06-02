# v2.5.0

First minor-version bump on the `v2.4-pulsar-plus` branch since 2.4.0
shipped. Integrates three community PRs from
[@benvanmierloo](https://github.com/benvanmierloo) that fix real HA-side
behaviour bugs, plus follow-ups for findings
[@peter-mcc](https://github.com/peter-mcc) surfaced on 2.4.3.

All verified live on the maintainer's Pulsar MAX. Plus testers please
report — the code paths are expected to work identically on Plus per
upstream references but haven't been physically exercised there yet.

## Community contributions

### Decode local BLE status enum instead of cloud codes
[PR #7](https://github.com/botts7/esp32-wallbox/pull/7) by **@benvanmierloo**

The 2.4.x status-code map was a mix of cloud `status_id` codes
(161 / 178-180 / 189-194 / 209-210 / …) and a partial local-enum set.
The actual BLE protocol returns a clean 0-18 enum on both MAX and
Plus per
[jagheterfredrik/wallbox-ble](https://github.com/jagheterfredrik/wallbox-ble).
The two clash at several codes — worst at **`st=6`: locally it means
LOCKED, in the cloud table it meant ERROR**. A locked charger was
rendering as "Error" + "car unplugged" in HA.

This release adopts the local enum everywhere — Charger Status sensor,
dashboard, notification triggers, car-connected logic. The
maintainer's MAX with Eco Smart enabled now correctly reads "Queued
(Eco-Smart)" (st=18) where previously it showed "Waiting for Schedule".

### Render HA Charging/Lock as real toggles
[PR #8](https://github.com/botts7/esp32-wallbox/pull/8) by **@benvanmierloo**

The Charging and Charger Lock MQTT switches rendered as a pair of
on/off buttons because HA's state never resolved — `payload_on`/
`payload_off` were the command strings (`start`/`stop`, `lock`/
`unlock`), but the value_template emits `"1"`/`"0"`. Added explicit
`state_on`/`state_off` to discovery so HA matches the template output
and renders proper sliding toggles.

### Auto Lock controls actually work
[PR #9](https://github.com/botts7/esp32-wallbox/pull/9) by **@benvanmierloo**

Three connected fixes:

- `g_alo` returns the timeout as a **bare scalar** (`{"r":60}`, `0` =
  off). 2.4.x mis-parsed it as an object — the Auto Lock switch
  stuck OFF, timeout always showed a default.
- `s_alo` also expects a bare scalar. HA was sending an object, so
  writes were silently ignored. Now sends the scalar; toggling ON
  restores the last-seen timeout instead of resetting to a default.
- The HA Auto Lock Timeout is now expressed in **minutes** (1-60) to
  match the Wallbox app, converted to seconds at the BAPI boundary.
  Rendered as an exact-value input box instead of a 60-step slider.

Manually ported to the current BLE-task architecture (PR #9 was
written against rc15-era main-task polling).

## Peter-mcc 2.4.3 follow-ups

### Persistent boot record in OTA history

`/info` now shows both upload events (`● version`) and successful
boots (`→ booted version`). Pairs the existing OTA-time recording
with a new `markHealthy()` entry capturing the version that reached
healthy state on this boot. Closes the gap Peter pointed to: "OTA
history should have 2.4.2 in it" — the upload event recorded by the
*old* firmware was always there, but there was no record confirming
2.4.2 actually booted.

### OTA history capacity 5 → 20

The 5-entry ring rolled past Peter's 2.4.2 entry after a couple of
subsequent OTAs. 20 entries cover a typical month of upgrade activity
without bloating NVS.

### Clear-counters button also resets `loop_max_ms`

The tripwire used to stick at its boot-time max until reboot. The
button now zeroes both `g_loopMaxMs` and the in-flight loop-gate
deadline alongside the existing BLE/MQTT reconnect counter reset.

## Other changes

### Auto Lock toggle bounce eliminated

Surfaced during 2.5.0 development. The fix: a 500 ms delayed re-poll
after every state-changing command, so HA receives the real charger
state shortly after the write returns. Bounce window collapsed to
fast-and-accurate feedback.

An "optimistic publish" path was added then reverted. Live testing
showed the visible "bounce" was the Wallbox charger acting
unilaterally — it auto-releases the socket lock after ~7 s when no
active session needs it, and it stays in st=18 (Queue Eco-Smart) when
Eco-Smart is gating charging regardless of manual Start. Painting
over those state transitions optimistically would lie to the user.
The repoll path that confirms reality within ~500 ms was kept; the
optimistic layer was removed.

### HA device discovery block consolidated

`populateDeviceBlock()` is now the single source of truth across all
6 discovery helpers. Every payload carries identifiers, name,
manufacturer, model, sw_version (live charger app FW), connections
(WiFi MAC), and configuration_url (deep link to gateway dashboard).

## Verification status

| Surface | Verified on |
|---|---|
| PR #7 status enum (st=18 → Queued Eco-Smart, st=6 → Locked) | Pulsar MAX |
| PR #8 switch toggle render | Pulsar MAX |
| PR #9 g_alo round-trip (0 → 60 → 0 → 300) | Pulsar MAX |
| Delayed-repoll bounce reduction | Pulsar MAX |
| Boot record in OTA history | Pulsar MAX (JSON + `/info` UI) |
| Clear-counters zeroes loop_max_ms | Pulsar MAX (`/info` Click) |
| All 2.5.0 fixes on Pulsar Plus | **Not yet — feedback welcome** |

## Flashing

OTA from 2.4.x (uses the auto-retry admission path from 2.4.1):
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```

USB:
```bash
python -m platformio run --target upload
```
