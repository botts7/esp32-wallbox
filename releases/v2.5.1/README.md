# v2.5.1

Root-cause fix following @peter-mcc's
[2.5.0 feedback](https://github.com/botts7/esp32-wallbox/issues/4): he
reported needing to press the "Clear counters" button three times
after upgrading before it took effect.

## What was wrong

The CSRF token was regenerated on every boot (random-seeded by MAC
+ millis + cycle counter at first `ensureCsrfToken()` call). A `/info`
page held open in the browser across an OTA reboot still held the
**old** token in its JavaScript (`window.WB_CSRF`). Every
state-changing fetch from that stale page silently 403'd from the
gateway with no UI feedback — making it look like the button was
broken. Users had to refresh the page (manually or by mashing the
button until something happened) before the new token kicked in.

## What's fixed

The token now **persists in NVS** (`wbcsrf:token`). Generated once
on first boot, stored forever after that. The browser's cached
`window.WB_CSRF` stays valid across reboots, so the stale-token
problem can't happen. No client-side recovery dance, no toast, no
auto-reload — just the right token in the right place.

Factory reset explicitly wipes the `wbcsrf` NVS namespace so a reset
device generates a fresh token instead of inheriting the previous
installation's.

A tight-race guard (`csrfTokenReady` flag) prevents two concurrent
first-boot requests from racing the NVS write and ending up with the
RAM copy diverging from the persisted one.

## Threat-model note

CSRF tokens are double-submit tokens, not session secrets. A
long-lived per-device random 128-bit hex value is fine — it's only
ever readable from an already-authenticated session (it's embedded
in pages rendered behind `checkAuth()`). The previous boot-rotation
gave no real security benefit and caused the UX bug above.

## Verification

Tested on my Pulsar MAX by reproducing Peter's scenario:
1. Opened `/info` in a browser
2. OTA'd a new firmware (gateway rebooted)
3. Clicked Clear counters from the now-stale page
4. Worked first try — no 403, no reload needed

## Flashing

OTA from 2.4.1+:
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```

USB:
```bash
python -m platformio run --target upload
```
