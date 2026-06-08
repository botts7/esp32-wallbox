# v2.4.0-rc9

**Probable Plus connect unlock.** Peter's rc7 GATT topology dump (issue #4)
revealed that Pulsar Plus uses a **Silicon Labs BGX13P** Bluetooth-to-UART
bridge module between the BLE link and the Wallbox firmware. The BGX has
a Mode characteristic — `75a9f022-af03-4e41-b4bc-9de90a47d50b`, confirmed
as the BGXSS Mode Characteristic in Silicon Labs' Android BGXpressService
reference code — which selects between:

- **STREAM_MODE (`0x01`)** — data passthrough; our BAPI bytes reach the
  Wallbox MCU behind the BGX
- **REMOTE_COMMAND_MODE (`0x03`)** — data interpreted as BGX commands;
  our writes silently discarded before reaching Wallbox

Default BLE bus mode depends on BGX firmware config. Some Plus units boot
into STREAM (jagheterfredrik/wallbox-ble worked on those), others boot
into REMOTE_COMMAND (Peter's Plus FW 6.7.38 on hardware FW
BGX13P.1.2.2045 — every BAPI write timed out on rc7 even though
notifications worked).

**The fix:** rc9 reads the BGX Mode characteristic on connect and writes
`0x01` if it's not already in STREAM_MODE. Plus-family only (MAX uses
u-blox single-char, no BGX).

## Diagnostic

If STREAM-mode still doesn't unlock the connection on some firmware, rc9
also logs the **raw bytes of any notification that isn't a BAPI frame**
— the BGX in command mode tends to send back text like `ERR\r\n` or
`ready\r\n` which the BAPI parser silently discards on earlier releases.
Format:

```
[BLE] RX raw (N): hex-bytes |ascii-bytes
```

Only fires for non-BAPI frames, so normal traffic stays quiet.

## Everything from rc7/rc8 still in place

- Adaptive PIN auth probe
- `r_dat` keepalive on Plus
- Plus 19-state status enum
- GATT topology dump at connect
- All OTA safeguards (admission guard, truncation check, boot counter,
  `/api/health`)
- In-RAM log buffer + `/api/logs` + `/logs` viewer
- `r_not` charger notifications → HA
- Config export / import
- Charger presets (MAX / Plus / Copper SB / Quasar / Quasar 2)
- FW change tracking
- Grounding diagnostic
- RSSI smoothing

## SHA256

See `SHA256SUMS.txt`.

## Installation

ESP Web Tools manifest at `install.json`. Existing rc7+ gateways can OTA
via `/ota` — the admission guard accepts the upload only when healthy.
For a hard-bricked rc5/rc6 gateway, USB-flash all three files via
`esptool.py`.
