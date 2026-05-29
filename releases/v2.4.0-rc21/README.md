# v2.4.0-rc21

Root-cause fix for the OTA-during-upload panic that has been hitting
testers since rc14 (peter-mcc #4) and that I reproduced locally on rc20
with a curl multipart POST. Plus boot-history hardening so the /info
"Last boot" badge reflects reality after a firmware upgrade.

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
