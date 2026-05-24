---
name: Pulsar Plus compatibility report
about: Help us add Pulsar Plus support
title: "Pulsar Plus compat: "
labels: compatibility, pulsar-plus
assignees: ''
---

Thanks for testing! Pulsar Plus support is in active development and your
data unlocks the next release. Fill in as much as you can — partial
reports are still useful.

## Hardware

- Charger model: Pulsar Plus
- Charger firmware version (from app or `r_dat`):
- ESP32 model (e.g. ESP32-S3-DevKitC-1, N16R8 with antenna):
- Gateway version: v2.3.0

## BLE Scan output

From `http://wallbox-gw.local/config` → BLE Scan, paste the row(s) that
look like your charger:

```
(address) (name) RSSI:(dBm) — services:[...]
```

## Telnet log during connection

`telnet wallbox-gw.local` from your computer, then save the BLE address
in the gateway and watch the log:

```
(paste from [BLE] Scanning... through [BLE] Ready or error)
```

## r_dat response (if you got that far)

`http://wallbox-gw.local/info` → use the BAPI tool, send `r_dat`:

```json
(paste the JSON response)
```

## What worked / didn't

- [ ] Device found on scan
- [ ] BLE connection succeeded
- [ ] Service discovered
- [ ] Notifications subscribed
- [ ] BAPI commands returned data
- [ ] Status / control entities populated in Home Assistant

## Anything weird

Free-form notes — anything that didn't match what the README implied.
