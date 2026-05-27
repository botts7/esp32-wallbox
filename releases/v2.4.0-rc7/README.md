# v2.4.0-rc7

Broader charger compatibility + diagnostic surface + production-grade OTA safeguards.

## New chargers (all alias Plus protocol)

The Settings → Charger Model dropdown now lists:

- **Pulsar MAX** (single-char) — reference path
- **Pulsar Plus** (dual-char)
- **Copper SB** (Plus protocol — experimental) — confirmed protocol parity via
  `jagheterfredrik/wallbox-mqtt-bridge`
- **Quasar / Quasar 2** (Plus protocol — experimental) — same family, V2H states
  already in the decode map
- **Custom** (set UUIDs yourself) — escape hatch for future firmware

The protocol family selector controls auth flow, keepalive method, and the
status-code map. MQTT device card + web UI display the chosen product name.

## Firmware change tracking

The gateway now persists the charger's GATT FW string (`dev_fw`) across reboots
and shows a banner on `/info` if it changes — useful for catching Wallbox
silent auto-OTAs that change behaviour overnight. First-boot baseline is
saved silently; banner only appears on actual changes. Dismiss button on the
banner clears the warning.

## GATT topology dump (diagnostic)

At every BLE connect, the gateway logs every GATT service + characteristic
the charger exposes, with each characteristic's properties (read / write /
notify). Pure telnet-log diagnostic — surfaces UUID rotations and new
endpoints immediately for future firmware compatibility work.

## Grounding status (universal)

`r_wel` (grounding) is now read at connect and surfaced in `/info` Charger
Details + `/api/status`. Universal safety diagnostic, works on both MAX and
Plus.

## UI fixes

- The configure-page field labelled "BLE PIN" is now **"Bluetooth Passcode"**
  with a help line pointing to where to find it in the Wallbox app. This was
  causing real confusion — the gateway's field name didn't match the term
  Wallbox uses in their app for the same value.
- The Save & Reboot page now polls the gateway every 2 seconds and
  auto-redirects to the dashboard when it comes back. No more dead spinner.

## OTA safeguards (production-grade)

This release adds a `wb_health` module that protects future OTA flashes from
the kind of bricking that's possible with a half-finished upload:

| Safeguard | What it catches |
|---|---|
| Boot counter | Repeatedly-failed boots — surfaced in serial log |
| Healthy mark | Only commits OTA partition when WiFi + uptime > 30s |
| `/api/health` endpoint | Returns 503 with reason when gateway not healthy — OTA tooling can poll before flashing |
| OTA admission guard | Rejects upload at FILE_START if not healthy; **partition is never erased** |
| Size sanity | Rejects upload larger than the OTA partition before erasing |
| Truncation check | Refuses to commit a partial OTA partition as bootable (was the real cause of the flash storm during rc7 development) |
| FILE_ABORTED handler | Explicit `Update.abort()` on client disconnect mid-upload — no half-committed otadata |
| Re-entrant lock | Blocks second OTA upload while one is in progress |

## Other

- `g_mac` parser handles MAX's flat-shape response (`wlan_mac`/`eth_mac` at
  top level)
- Fixed inferred BAPI constants per `jagheterfredrik/wallbox-ble`:
  `r_dch`→`r_dis`, `r_ver`→`fw_v_`

## SHA256

See `SHA256SUMS.txt`.

## Installation

ESP Web Tools manifest at `install.json`. Existing gateways on v2.3.0+ can
OTA via `/ota`. **rc7 is the first release where the OTA path is properly
guarded** — earlier releases will accept any upload regardless of state,
which can brick the device under specific conditions (high-frequency
back-to-back OTAs while BLE is mid-reconnect). Upgrading to rc7 closes that
window.
