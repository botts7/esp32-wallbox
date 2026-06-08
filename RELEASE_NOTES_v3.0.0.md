# 3.0.0 — async + websocket + HA-native trio

A platform release that closes out the "block the main task while
talking BLE" pattern that's haunted the gateway since 1.x, and ships
two companion repos so HA users don't need an MQTT broker.

## What's new

### Async HTTP everywhere
The HTTP surface ran on Arduino's bundled `WebServer` since the
beginning — single-threaded, blocking, fragile under HA's burst
patterns. 3.0 migrates the entire surface to
`mathieucarbou/ESPAsyncWebServer@3.6.0` over 10 staged steps. The
sync server is retired in STA mode; AP mode keeps it as a
provisioning fallback.

### WebSocket pushes
The dashboard no longer polls `/api/charger` + `/api/command?action=bapi&met=r_dca`
on a 1 Hz timer. It opens `ws://host/ws` and receives push messages
keyed on `t`: `status`, `meter`, `settings`, `ble`. The WebSocket
handler is attached to the same async listener as HTTP, so no second
TCP port.

### Schedule writes finally work
The schedule write path silently mis-built the BAPI payload since
v2.1.0 — wrong method name, wrong field shape, no array wrapper.
Schedules created from the dashboard now round-trip correctly and
the charger actually obeys them.

### Power-flow visualization
The dashboard now shows a Grid → Charger → EV power-flow card with
animated direction arrows, current values per segment, and a
battery indicator. Matches the Wallbox app's visual language.

### Companion HA Add-on
[`botts7/wallbox-gateway-ha-addon`](https://github.com/botts7/wallbox-gateway-ha-addon) v0.2.0 — install it from the HA Add-on
Store as a Local Add-on. Surfaces the gateway dashboard as a sidebar
panel (via Supervisor ingress, no port-forwarding), plus drag-and-drop
OTA firmware upload with end-to-end MD5 verification.

### Companion HA Integration
[`botts7/hass-wallbox-gateway`](https://github.com/botts7/hass-wallbox-gateway) v0.2.0 — install it
via HACS as a custom repository (or copy `custom_components/` into
your HA config). Single-step config flow, 13 entities (6 sensors +
2 binary + 3 switches + 1 number + 1 select + 1 button). No MQTT
broker required.

## Upgrading

OTA upgrade from the Add-on dashboard's `/ota` page (recommended), or
manual: `pio run -e ota -t upload --upload-port <gateway-ip>`.

Settings, schedules, and BLE credentials all persist across the
upgrade — no factory reset required.

## Backwards compatibility

- MQTT discovery continues to publish the same topic shape as 2.7.x;
  existing HA setups that rely on the MQTT path keep working.
- The `/api/command?wait=N` and `/api/command_status?id=N` endpoints
  from 2.7.0 are unchanged.
- The `WB_ASYNC_WEB=0` build flag still produces a working sync-only
  firmware for callers that depend on the exact byte-for-byte sync
  response shape.

## Full changelog

See [`CHANGELOG.md`](CHANGELOG.md#300---2026-06-07).
