# v2.4.0-rc11

Security hardening pass + threat-model documentation. **If your gateway
is on a network you don't fully control, this is the upgrade.**

## Critical security fix — OTA endpoint now auth-gated

rc10 and earlier accepted firmware uploads at `POST /api/ota` with no
authentication. Anyone on the same WiFi could flash arbitrary firmware —
brick the device, install a backdoor, replace it with their own build.
rc11 adds a `checkAuth()` check at FILE_START, before the admission
guard, before `Update.begin()` erases the partition.

The check is a no-op when web auth is disabled (matching the existing
UX), so this is non-breaking for setups that don't enable auth. Setups
*with* auth enabled now get proper protection.

## Other endpoints hardened

| Endpoint | Was | Now | Impact if unguarded |
|---|---|---|---|
| `POST /api/ble/pause` | open | auth-gated | DoS the BLE link from anywhere on the LAN |
| `POST /api/fw/dismiss` | open | auth-gated | Minor — consistency fix |
| `GET /api/wifi-scan` | open | auth in STA mode only | Discloses nearby SSIDs (still open in AP setup mode) |

The full audit, including what's deliberately *not* protected (read-only
telemetry endpoints, the broadcast-only WebSocket, mDNS) is documented
in the new [SECURITY.md](../../SECURITY.md) at the repo root.

## Threat model — new SECURITY.md

First written-down threat model. Covers:

- Who the gateway is designed to defend against (LAN clients) vs. trust
  (the user themselves, the WiFi router, the MQTT broker)
- Per-endpoint auth/CSRF table
- What's *not* protected and why (read-only API, broadcast WS, mDNS,
  no HTTPS)
- Known limitations (NVS plaintext, AP-mode setup-window, no TLS)
- Mitigations for each limitation

## Documentation — two architectures clarified

The README's Architecture section now documents both valid integration
paths:

- **This project** — smart gateway, BAPI in C++ on the ESP32, MQTT to HA
- **HA Bluetooth Proxy + [`jagheterfredrik/wallbox-ble`](https://github.com/jagheterfredrik/wallbox-ble)** — dumb radio relay + Python HA component

Both work. The comparison table makes the trade-offs explicit so users
pick what fits their setup.

## Everything from rc10 still in place

BGX13P stream-mode unlock (Plus), OTA safeguards, GATT introspection,
log buffer, charger notifications, config export/import, OTA history,
backup/restore UI, RSSI smoothing — all unchanged.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (which now requires auth if you
have it enabled — make sure you're logged in before uploading). Fresh
USB installs use `install.json` via ESP Web Tools.

## Strongly recommended after upgrade

If you haven't already, enable **Web Authentication** on `/config` and
set a password. With auth enabled, rc11's new checks actually do
something — without it, they're no-ops.
