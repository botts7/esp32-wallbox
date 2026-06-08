# v2.6.0

Architectural fix for the main-loop wedge @peter-mcc reported on
2.5.1: his `loop_max_ms` metric showed **80 000 ms** overnight,
root-caused to the HA-discovery burst hitting a stalled MQTT broker
and compounding ~60 sync TCP writes in series before the broker
recovered.

## The fix

`WallboxMQTT::sendDiscovery()` used to publish all ~57 HA
auto-discovery entities back-to-back in one main-loop iteration —
under a healthy broker that took ~600 ms; under a stalled broker
each individual publish blocked until the TCP socket timeout, and
the timeouts compounded.

After 2.6.0:

- `sendDiscovery()` ARMS a state machine and returns immediately
  (`_discoveryIndex = 0`)
- A new `WallboxMQTT::tickDiscovery()` is called from the main loop
  once per iteration; each tick publishes EXACTLY ONE entity from a
  flat switch keyed on the index
- WiFi client socket timeout dropped from the 5 s default to 1 s

Result: under a stalled broker the per-loop cost is bounded to one
socket timeout (~1 s) instead of compounding to tens of seconds.
Live-validated on a Pulsar MAX:

| Build | `loop_max_ms` |
|---|---|
| v2.5.0 worst case | 4 156 ms |
| v2.5.1 typical | 295-866 ms (boundary leak fixed in 2.5.1) |
| v2.5.1 peter-mcc overnight | 80 000 ms |
| **v2.6.0** | **29-30 ms** |

The discovery burst itself completes in ~600 ms wall-clock under a
healthy broker (existing `delay(10)` × 57 ticks). HA's autodiscovery
is per-message so partial bursts are safe; entities populate
gradually as ticks land.

## Other changes

### HA Device page reorganised

~12 debug-only sensors now carry `entity_category: diagnostic` so
HA collapses them into a separate "Diagnostic" section on the
device page instead of mixing with user-facing sensors. peter-mcc
2.5.1 feedback on what's "Reentry Tripwire" and similar metric
clutter.

Affected: Loop Max ms, Heap Free, Heap Min Watermark, Reentry
Tripwire, Rate-Limit Tokens, Gateway Uptime, Last Boot Reason,
BLE Paused, BLE Signal, WiFi Signal, Charger Grounding, Gateway
Firmware.

### Dropped "Status Code (raw)" sensor

Was exposing `r_sta.charger_status` as a raw integer with an
undocumented enum mapping. The friendly "Charger Status" sensor
from PR #7 is the canonical user-facing value. Existing HA installs
get a one-time cleanup — `sendDiscovery()` publishes an empty
retained payload to the old discovery topic on arm, which is HA's
documented "delete this entity" mechanism.

## Verification on Pulsar MAX

- All ~56 entities populate in HA after a fresh OTA
- "Diagnostic" section renders correctly with the 12 entities listed
  above; main "Sensors" section is shorter
- Charger Firmware sensor flips from gateway-fw fallback to
  6.11.16 within ~1 s of BLE init completing (proves the state
  machine re-arms correctly on `discoveryStale`)
- `loop_max_ms` stays at 29-30 ms steady-state
- Toggle paths (Auto Lock, Charging, Lock) round-trip cleanly

Not yet exercised: wedged-broker simulation. The architectural
guarantee (one publish per tick) means worst-case is mathematically
bounded — but if anyone wants to stress-test with `iptables` /
firewall-blocking the broker, that'd be a useful confirmation.

## Architecture note

This is the third "move blocking work off the main loop" change on
this branch:
- **rc16** moved BLE polling onto a FreeRTOS task
- **2.4.3** gated `loop_max_ms` during reconnect windows
- **2.6.0** lifts the HA discovery burst out of the main loop's hot path

Remaining main-loop blockers identified by the 2.5.1 audit and
deferred: `WiFi.reconnect()` blocking 15-30 s on a stuck AP, and
`/api/command` BLE-passthrough blocking up to 5 s. Smaller and more
targeted; queued for future releases if real-world wedges keep
landing on them.

## Flashing

OTA from 2.4.1+:
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```

USB:
```bash
python -m platformio run --target upload
```
