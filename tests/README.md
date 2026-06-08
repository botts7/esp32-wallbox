# Tests

Live integration tests run against a real ESP32-S3 gateway over the
network. They use `requests` and assume the gateway is reachable at
the env-var-configured IP with web auth credentials.

## Setup

```bash
pip install requests websocket-client paho-mqtt
export WB_GATEWAY=http://192.0.2.1    # your gateway IP, no trailing slash
export WB_AUTH_USER=admin
export WB_AUTH_PASS=...                # OTA password from /info or serial log

# Only needed for tests.longevity.TestMqttCommandStorm (otherwise skipped):
export WB_MQTT_BROKER=broker.local:1883
export WB_MQTT_USER=...
export WB_MQTT_PASS=...
export WB_MQTT_PREFIX=wallbox          # optional; default is "wallbox"
```

Run an individual suite:

```bash
python -m unittest tests.api_command_async -v
python -m tests.hardening
python -m tests.ui_surface
```

Or sweep everything:

```bash
python -m unittest tests.api_command_async tests.edge_cases tests.longevity -v
python -m tests.ui_surface && python -m tests.hardening
```

## Why integration over unit

The blocking-call behaviour these tests verify is observable only
when wired against a real BLE+MQTT+WiFi stack. Unit-testing
`WallboxBLE::enqueueRequest` in isolation would mock the queue
plumbing and miss the very races the refactor is trying to remove.
The tests double as a regression harness during the multi-step
2.7.0 refactor.

## Test files

- `api_command_async.py` (13 tests) — exercises `/api/command`
  request/response semantics, latency bounds, 202 + `command_status`
  poll path, sync escape hatch, baseline contract. Anchors the
  2.7.0 refactor (see `docs/plans/2.7.0-api-command-async.md`).
- `edge_cases.py` (13 tests) — parallel correlation under concurrency
  (proves the response map + xTaskNotify don't cross wires under
  6-way burst), URL clamps and garbage inputs, token bucket /
  queue overflow recovery. Found a real `?wait=0` regression
  during 2.7.0 development that the baseline tests didn't catch.
- `longevity.py` (4 tests) — 90-second memory soak with periodic
  burst (heap_free stability), WebSocket resilience under HTTP
  load (chunked-wait pump verification), MQTT command storm
  (opt-in via WB_MQTT_* env vars; verifies the Step-8 fire-and-
  forget path doesn't drop bursts).
- `hardening.py` (script, not unittest) — reusable burst + soak
  harness with a background charger-monitor thread that polls
  `r_dat` to verify the EVSE itself stays healthy throughout
  the test. Per-step regimen during the 2.7.0 refactor.
- `ui_surface.py` (18 probes) — mirrors every read-path BAPI
  fetch the web UI's JS makes. Catches GUI-contract drift that
  the API-only tests miss (e.g. a server change that returns a
  different JSON shape than the modal's `.then()` handler expects).

## What they DON'T do

- They don't OTA-flash. Build + flash via PlatformIO before running.
- They don't reset gateway state. Failing tests can leave retained
  MQTT messages or NVS state in odd shapes — re-flash if a test
  pollutes the device.
- They don't reach BLE-radio behaviour. A wedged BLE link will fail
  these tests with timeouts; that's not a test bug.
