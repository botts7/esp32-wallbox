# v2.4.0-rc22

Pure observability addition on top of rc21 — no behaviour change, no
bug fixes. Surfaces the rc20/rc21 diagnostic fields via MQTT so Home
Assistant users can monitor them without having to poll
`/api/health` from a shell.

## Why

The rc21 release added two important fields that prove the
reentrancy fix is holding:

- `max_reentry` — must stay at 1. Anything higher means
  `handleClient()` was pumped re-entrantly somewhere (the exact bug
  class that caused every sustained-load panic from rc14 to rc20).
- `tokens` — current `/api/command` rate-limit bucket level. A
  persistently low value means a client (HA, dashboard, or scripted
  load) is being throttled.

Both were only on `/api/health`. HA users couldn't alarm on them,
graph them, or include them in fleet health dashboards. rc22 fixes
that.

## What's in the gateway-info MQTT topic now

`wallbox/response/gateway` (60-second cadence — same as before)
gains:

- `fw` — gateway firmware version (`WB_VERSION`, e.g. `v2.4.0-rc22`)
- `max_reentry` — rc21 reentry tripwire (must stay 1)
- `tokens` — rc21 token-bucket remaining
- `boot_reason` — current boot's `esp_reset_reason()` string
- `heap_min_ever` — fragmentation watermark (worst-case free heap)
- `ble_paused` / `ble_pause_remaining_s` — "Released BLE for App" state
- `chg_grounding` — already on `/api/status`, missing on MQTT until now

## New HA discovery entities

All registered automatically on next MQTT connect. No manual sensor
config needed.

| HA entity | Backed by |
|-----------|-----------|
| Gateway Firmware | `fw` |
| Last Boot Reason | `boot_reason` |
| Reentry Tripwire | `max_reentry` — **alarm if > 1** |
| Rate-Limit Tokens | `tokens` |
| Heap Free | `heap` |
| Heap Min Watermark | `heap_min_ever` |
| Gateway Uptime | `uptime` |
| BLE Paused | `ble_paused` + `ble_pause_remaining_s` |
| Charger Grounding | `chg_grounding` |
| WiFi Signal | `wifi_rssi` |

## Upgrading from rc21

OTA via `/ota`. No NVS migration, no reconfiguration. The new HA
entities appear automatically once the gateway reconnects to your
broker and publishes discovery (look for `[MQTT] HA discovery
published` in the gateway log).

If you just installed rc21, the new entities will appear after the
first 60-second `publishGatewayInfo` tick.

## Plain English: should I upgrade?

Yes if you use Home Assistant and want the rc21 observability fields
to be alarmable / graphable in HA. No urgency otherwise — rc21 is
functionally complete.

## SHA256

See `SHA256SUMS.txt`.

## Carries forward from rc21

- Re-entrant `handleClient()` panic fix
- OTA-during-upload panic fix (BLE pause race)
- Boot history with fw-version tagging
- XSS hardening on scan results
- Boot/nav overlay UX
- Token bucket rate limit
- Universal `/api/command` retry with backoff
- /settings truncation fix (chunked sendContent)
- PIN UX (dual-fact: gateway-stored + charger-expected)
- Auto Lock dual-shape handling
- WiFi status enrichment
- `scripts/stress_api.py`, `scripts/probe_bapi.py`
