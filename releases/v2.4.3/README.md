# v2.4.3

Tightens the `loop_max_ms` tripwire so it stops false-positiving on
sync MQTT and BLE reconnect blocking. Driven by @peter-mcc's testing
of 2.4.2.

## What was wrong

`PubSubClient` (the MQTT library on the gateway) is synchronous and
runs on the main task. When the broker briefly goes away — an HA
MQTT integration reload, a router hiccup — the next `MQTT.loop()`
blocks while `connect()` retries. The default timeout is ~15 s per
attempt, so a couple of failed-then-recovered cycles can produce a
30-40 s loop gap.

The `loop_max_ms` tripwire (added in 2.4.1) faithfully measured
that. But the metric was meant to catch *unprovoked* runtime
wedges — the peter-mcc #4 hours-of-uptime freeze class — not
predictable sync-blocking during reconnect events. Once the value
saturated, any real wedge underneath was invisible.

@peter-mcc's gateway showed `loop_max_ms = 40 082 ms` alongside two
MQTT disconnect events that he attributed (correctly) to him
"stuffing around with HA". The metric was working; the framing
wasn't.

## What's fixed

New `wb_diag::extendLoopMaxGate(graceMs = 30000)` API. Called
automatically from `reportReconnect()` — the existing single-source-
of-truth for tracked BLE and MQTT reconnect events. Every successful
reconnect pushes a 30 s grace window forward. The main loop's gap
tracker consults `loopMaxGateActive(now)` and skips recording during
that window.

Overlapping reconnects only extend the window, never shorten it.
The gate auto-clears on expiry so the next event re-arms cleanly.

## Layered filters now stacked on `loop_max_ms`

After this release, three filters stack on the metric. What lands in
`loop_max_ms` after all three fire is a genuine unprovoked wedge:

1. **First 60 s of uptime** (2.4.1) — boot-phase MQTT discovery flood
2. **30 s after any tracked BLE/MQTT reconnect** (this release)
3. The trivial "first-iteration timestamp is meaningless" check

## Live-validation

Tested on the maintainer's MAX by stopping and restarting the
Mosquitto broker:

| Metric | Before MQTT restart | After |
|---|---|---|
| `loop_max_ms` | 31 ms | 265 ms |
| `mqtt_reconnects` | 0 | 1 |
| `mqtt_longest_s` | — | 10 |

Without the gate that 10 s outage would have produced 10 000+ ms.
With the gate it surfaces as 265 ms — comfortably under the 500 ms
yellow threshold. Heap, BLE, and 2.4.2 fields all intact through and
after the test. 24 minutes of steady operation post-test; the
metric did not climb further.

## Flashing

OTA from 2.4.2 (uses the 2.4.1 auto-retry admission path):
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```

USB:
```bash
python -m platformio run --target upload
```
