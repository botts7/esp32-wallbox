# v2.4.0-rc6

Pulsar Plus protocol pass + RSSI stabilisation + universal serial / MAC read.

## Pulsar Plus (derived from `jagheterfredrik/wallbox-ble`)

- **Adaptive auth probe.** rc5 sent `read_pin` and silently declared "no PIN
  needed" when the charger didn't reply — that's exactly the case for older
  Plus firmware, which leaves the gateway thinking it's authenticated while
  every subsequent BAPI write is dropped (the "TX with no RX" symptom).
  rc6 tries `read_pin`, and if that stays silent it probes with `r_dat` to
  confirm the channel actually works before declaring success.
- **Keepalive on Plus uses `r_dat`.** `ping` isn't in the Plus method table,
  so rc5's 30-second keepalive would fail every cycle and trigger a
  reconnect loop forever.
- **Plus status code map (0-18).** Plus uses a clean enum
  (`READY/CHARGING/WAITING_FOR_CAR/...`), totally different numbering from
  MAX's sparse hardware codes. The HA discovery template and web SN map
  now swap based on the model selected in Settings.
- **200 ms wait after `secureConnection()`.** NimBLE 1.4.1 can return
  before the bond info fully lands, so the notify retry could race.
- **MQTT device card model-aware.** No more hardcoded "Wallbox Pulsar MAX"
  showing up under a Plus.

## Universal (MAX + Plus)

- **Charger serial (`r_sn_`) and MAC (`g_mac`)** read at connect, surfaced in
  the `/info` Charger Details card and `/api/status` JSON.
- **Fixed BAPI constants** that were inferred but wrong: `r_dch` → `r_dis`,
  `r_ver` → `fw_v_`.

## RSSI (closes #6)

- Single-source EMA. Sampling happens once per 2-second cycle inside
  `wallboxBLE.loop()`; `rssi()` is now a pure getter so every consumer
  (web banner, Gateway-card row, MQTT gateway topic, WS push) reads the
  same number at any given instant.
- Top BLE banner subscribes to the same `ble` WS push the Gateway card
  uses — both update in lockstep on every refresh, no drift.
- Fixed `window.wbws.subscribe()` so multiple handlers can register for
  the same topic (was overwriting — the banner subscription was getting
  clobbered when later page-body scripts subscribed).

## Other

- Empty-MAC guard in `_connect()` — no `[BLE] Scanning for ...` noise in
  the post-Save / pre-reboot window (closes #5).

## SHA256

See `SHA256SUMS.txt` next to the binaries.

## Installation

ESP Web Tools manifest at `install.json` — click the Install button on the
[main README](https://github.com/botts7/esp32-wallbox/blob/main/README.md)
or follow [INSTALL.md](https://github.com/botts7/esp32-wallbox/blob/main/INSTALL.md)
for manual install. Existing gateways on v2.3.0+ can OTA via `/ota`.
