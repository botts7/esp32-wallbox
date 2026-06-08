# v2.4.0-rc15

**BLE state machine moved to its own FreeRTOS task** — fixes the UI lag, slow
page loads, OTA upload failures, and variable ping latency that both
botts7 and peter-mcc reported.

## What was wrong

Until rc14, every BLE operation ran on the Arduino main loop. That meant:

- A BLE scan (5 seconds) froze the web UI completely
- A reconnect attempt (10+ seconds) froze it longer
- A chain of BAPI calls (e.g. `pollSettings`'s 5 sequential reads, up
  to 15s blocking) froze it intermittently
- During those freezes, ping latency to the gateway spiked from 20 ms
  to 400+ ms while neighbouring devices (HA, etc.) stayed stable

rc14 fixed one specific symptom (MQTT keepalive starvation, clockwork
85-second disconnect cycle). But the underlying class of problem —
*BLE work blocking everything else* — remained.

## The fix

The BLE state machine (scan / connect / auth / keepalive ticking) is
now driven by a dedicated FreeRTOS task pinned to Core 1, running at
~50 Hz. The Arduino main loop no longer calls `wallboxBLE.loop()`.

`sendCommand()` stays synchronous for backward compatibility — every
existing caller works unchanged. A mutex (`_cmdMutex`) serialises
concurrent callers so the BLE task's own keepalive can't race against
the main task's polls.

Verified locally: while the gateway is mid-BLE-scan + connect cycle
on first boot, `/api/status` responses stayed 100-230 ms (previously
they'd hang for 5+ seconds during scan).

## What this doesn't yet fix

`sendCommand()` is still blocking from the caller's perspective —
when the main task calls it for `pollStatus`, that task waits up to
5 s for the BAPI response. Other tasks keep running, but the polling
chain itself is still serial.

If after rc15 you still see UI lag during specific operations, that's
the cue for **Phase 2** (rc16: async sendCommand with callback or
result cache), which makes the main task fire-and-forget on BAPI
requests. We'll ship that if rc15 isn't enough.

## Everything from rc14 still in place

- MQTT keepalive starvation fix (MQTT.loop() in BLE yield + 60s keepalive)
- All rc7-rc13 safeguards, diagnostics, UX

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11).
Fresh USB installs via `install.json`.
