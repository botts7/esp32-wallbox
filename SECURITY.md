# Security Model

## Threat model

The Wallbox Gateway is designed for **LAN deployment behind a trusted home
network**. The threat model assumes:

- The ESP32-S3 is physically inside the user's home (not in a public space)
- WiFi access requires the user's network password
- The MQTT broker, if used, is on the same LAN and itself trusted
- An attacker who already has physical access to the device can read NVS
  and recover stored credentials — that's outside scope

It does **not** assume:

- The LAN is hostile (every device on WiFi is a potential attacker)
- The LAN is friendly (default-allow everything between LAN clients)

So the gateway provides **authentication on every state-changing endpoint**,
rate-limited login attempts, and CSRF protection — sufficient for a
trusted-network deployment with mistrust between LAN clients. It does
**not** provide transport encryption (HTTPS) and is **not** suitable for
direct exposure to the internet.

## What's protected

| Endpoint | Auth | CSRF | Reason |
|---|---|---|---|
| `POST /save` (config update) | ✓ | ✓ | Can change WiFi, MQTT, BLE credentials |
| `POST /reset` (factory reset) | ✓ | ✓ | Destructive |
| `POST /api/ota` (firmware upload) | ✓ | — | Could brick or backdoor the gateway |
| `POST /api/config/import` | ✓ | ✓ | Restores arbitrary config |
| `GET /api/config/export` | ✓ | — | Discloses config (passwords masked) |
| `POST /api/command` (arbitrary BAPI) | ✓ | — | Can control the charger |
| `POST /api/ble/pause` | ✓ | — | DoS the BLE link |
| `POST /api/fw/dismiss` | ✓ | — | Clears the FW-change banner |
| `GET /api/wifi-scan` (STA mode) | ✓ | — | Discloses nearby SSIDs |
| `GET /api/logs`, `GET /logs` | ✓ | — | Log contents may leak data |
| `GET /api/ota/history` | ✓ | — | Reveals OTA history |

Auth is **HTTP Basic Auth** over the LAN. It is enabled via the *Web
Security* card on `/config`. When enabled:

- Rate-limited: 5 failed attempts → 30-second lockout per IP-ish window
- Each failed attempt adds a 1-second delay to slow brute force
- The same credentials authorise `espota` push uploads

The gateway shows persistent warning banners on the dashboard if web
auth is disabled.

## What's *not* protected (and why)

| Endpoint / surface | Auth | Why |
|---|---|---|
| `GET /api/status` (status JSON) | — | Read-only telemetry; HA / monitoring tools poll it without creds |
| `GET /api/charger` (cached status) | — | Same — read-only telemetry |
| `GET /api/health` | — | Health-check endpoint by design; used by uptime monitors |
| `GET /api/ble-scan` | — | Used during initial setup before any creds exist |
| `GET /api/wifi-scan` (AP mode) | — | Used during initial setup |
| `WebSocket :81` | — | Broadcast-only — same info as `/api/status`, no command surface |
| `mDNS broadcast (port 5353)` | — | Advertises hostname + version. Same as any local device. |

If any of these matter for your environment, **enable HTTP auth and put
the gateway on a VLAN with restricted client-to-client communication**.

## Known limitations

### No HTTPS / TLS

All HTTP traffic is in cleartext over your LAN. Anyone with network
visibility can sniff Basic Auth credentials, BAPI commands, and status
data. For most home deployments this is acceptable because:

- The LAN is trusted
- No data crosses the internet
- The MQTT broker itself usually has auth, providing a second layer for
  HA users

If you need encrypted transport, options are:

- Run the gateway on a dedicated VLAN with no other clients
- Front it with a reverse proxy (Nginx, Caddy) on a trusted host that
  terminates TLS
- Self-signed certs on the ESP32 itself — possible but cert renewal /
  rotation is painful; not implemented

### AP-mode initial setup uses open WiFi

During the first-boot captive portal (before WiFi is configured), the
gateway advertises an open `WallboxGW` SSID. Anyone in radio range can
connect and view the setup form during this 5-minute window. After WiFi
credentials are saved and the gateway connects to the user's network,
AP mode shuts down.

Mitigation: complete the initial setup quickly. If WiFi setup must
happen in a high-density area (apartment building etc.), use a USB
serial connection to seed config directly into NVS via a build-time
flag instead of going through the AP.

### NVS plaintext storage

WiFi passwords, MQTT passwords, the Bluetooth Passcode, and the web auth
password are stored unencrypted in NVS. An attacker with physical access
to the ESP32 can read them via `esptool.py read_flash`. ESP32-S3 supports
**flash encryption** which would defeat this attack, but the feature is
one-way (can't be undone) and complicates firmware updates — not enabled
by default. Enable in `platformio.ini` if your threat model demands it.

### Brute-force on /save's CSRF token

CSRF token is 16 hex chars (64 bits). Practically unguessable, but
rotates only on reboot — long-lived sessions reuse the same token.

### `/api/command` doesn't enforce CSRF

It's auth-gated, but a malicious page that the authenticated user
visits could potentially issue a GET to it. Browser same-origin policy
prevents reading the response, but the side-effect (a BAPI call) goes
through. Typical impact: someone tricks you into starting/stopping
charging. Low severity given the threat model.

## Reporting a vulnerability

Open a security issue on GitHub
(<https://github.com/botts7/esp32-wallbox/security>) or email a maintainer
privately if it warrants responsible disclosure.

## What changed in rc11

rc10 had an unauthenticated `/api/ota` endpoint — anyone on the WiFi
could flash arbitrary firmware. Closed in rc11. Also added auth to
`/api/ble/pause`, `/api/fw/dismiss`, and (in STA mode) `/api/wifi-scan`.
Threat model documented in this file for the first time.

**Upgrade to rc11 if your gateway is on a network you don't fully
control.**
