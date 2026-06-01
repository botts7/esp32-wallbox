# v2.4.2

Follow-up release fixing two bugs in 2.4.1 that @peter-mcc surfaced
during his testing on a Pulsar Plus, plus a discovery-consistency
refactor that fell out of investigating them.

## What's fixed

### Total Charging Sessions sensor wasn't the lifetime count

2.4.1 surfaced `r_ses.size` as "Total Charging Sessions". That's
actually the log buffer capacity sentinel — 99 999 on the
maintainer's MAX, missing entirely on Plus. The real lifetime
count is in `r_ses.last` (verified 233 on the MAX). Switched to the
correct field. Kept `size` as a fallback but only when in a plausible
range so the sentinel can't be picked up. Chargers that expose
neither field publish `null` so the HA sensor goes unavailable
instead of sticking at 0 or -1 forever.

### HA Device-page firmware label was hardcoded

2.4.1's discovery device block had `dev["sw_version"] = "6.11.16"`
as a hardcoded literal — so every Pulsar (Plus, Quasar, Copper)
showed `6.11.16` on its HA Device page even though the per-entity
"Charger Firmware" sensor was correct. The device label is now
driven by `wallboxBLE.chargerAppFirmware()` with a `WB_VERSION`
fallback for the boot window. BLE init raises a one-shot flag once
`fw_v_` returns, and the main loop re-publishes HA discovery once so
the device label updates in place without a HA restart.

### Discovery device block consolidation

While digging into the firmware label, found that the six discovery
helpers (entity / switch / number / button / select / car-connected
binary_sensor) each built the `device` block inline and they'd
drifted. Only the entity helper carried `sw_version`. HA merges all
discovery device blocks by `identifiers`; with inconsistent blocks
the merged result flickered.

`populateDeviceBlock()` is the new single source of truth. Every
discovery payload now carries:

- `identifiers`, `name`, `manufacturer`, `model`, `sw_version`
- `connections: [["mac", <wifimac>]]` — surfaces the gateway's MAC
  in the HA Device page header alongside the firmware label
- `configuration_url: http://<gw-ip>/` — adds a "Visit" button to
  the HA Device page header that deep-links to the gateway dashboard

## Migration

If you're upgrading from 2.4.1 and your HA Device page still says
"Connected via Unnamed device" or shows stale entities (`Charger
WiFi Signal: -127 dBm` from the dev-build rename window), the
cleanest reset is:

1. Settings → Devices & Services → MQTT → click the Wallbox device
2. Delete it
3. From `/info` on the gateway, click "Reboot Gateway"
4. Wait ~30 s — the device reappears via fresh retained discovery
   payloads with all 2.4.2 fields populated

Once the device has a full `connections` array, HA stops linking it
under the auto-created MQTT broker hub and shows it as a
standalone device.

## Live-validation

Tested end-to-end on a Pulsar MAX:

- `chg_sessions` flipped from `99999` to `233` (the real count)
- HA Device firmware label flipped from `WB_VERSION` fallback to
  `6.11.16` once BLE init read `fw_v_`
- MAC and "Visit" button surfaced in the HA Device page header
- All other 2.4.1 fixes (lock inversion, WiFi signal as %,
  loop tripwire boot-skip) confirmed intact

## Flashing

OTA from the existing 2.4.1 (uses the auto-retry admission path):
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```

USB:
```bash
python -m platformio run --target upload
```
