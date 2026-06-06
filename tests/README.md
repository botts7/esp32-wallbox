# Tests

Live integration tests run against a real ESP32-S3 gateway over the
network. They use `requests` and assume the gateway is reachable at
the env-var-configured IP with web auth credentials.

## Setup

```bash
pip install requests
export WB_GATEWAY=http://192.0.2.1    # your gateway IP, no trailing slash
export WB_AUTH_USER=admin
export WB_AUTH_PASS=...                # OTA password from /info or serial log
```

Then run the test you care about, e.g.:

```bash
python -m tests.api_command_async
```

## Why integration over unit

The blocking-call behaviour these tests verify is observable only
when wired against a real BLE+MQTT+WiFi stack. Unit-testing
`WallboxBLE::enqueueRequest` in isolation would mock the queue
plumbing and miss the very races the refactor is trying to remove.
The tests double as a regression harness during the multi-step
2.7.0 refactor.

## Test files (current scope)

- `api_command_async.py` — exercises `/api/command` request/response
  semantics, latency bounds, and the upcoming 202 + `command_status`
  poll path. Used during the 2.7.0 refactor (see
  `docs/plans/2.7.0-api-command-async.md`).

## What they DON'T do

- They don't OTA-flash. Build + flash via PlatformIO before running.
- They don't reset gateway state. Failing tests can leave retained
  MQTT messages or NVS state in odd shapes — re-flash if a test
  pollutes the device.
- They don't reach BLE-radio behaviour. A wedged BLE link will fail
  these tests with timeouts; that's not a test bug.
