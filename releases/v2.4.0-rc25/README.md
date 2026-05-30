# v2.4.0-rc25

Cosmetic fix for false "Grounding fault" alarm on Pulsar Plus.

## What was wrong

peter-mcc reported on issue #4 that his Plus board's /info page
was showing "Grounding: Fault" while the charger operated normally
and the Wallbox app reported no issues.

Root cause: our integer mapping in `wb_ble.cpp:507` hardcoded the
MAX convention — `0 = OK, anything else = Fault`. His Plus's
`r_wel` BAPI returns `1` as its nominal healthy state.

Web research confirms `r_wel` is almost certainly Wallbox's
contactor-weld-detection ("welding protection" is marketed as a
Pulsar safety feature). The argument by contradiction is decisive:
if `r:1` meant "welding detected", the safety system would lock
the charger out and Peter wouldn't be able to charge. Since he
can, `r:1` on Plus is a healthy state encoded differently from
MAX — likely "sensor armed and monitoring" or similar.

## What rc25 changes

Drops the speculative non-zero → "Fault" mapping. Keeps the
verified `0 → "OK"` for MAX. Anything else renders as `Code N`
neutral framing.

| What MAX returns | Display | What Plus returns | Display |
|------------------|---------|-------------------|---------|
| `r: 0` | "OK" | `r: 0` | "OK" |
| `r: 1` | "Code 1" | `r: 1` | "Code 1" (was "Fault") |
| `r: N` | "Code N" | `r: N` | "Code N" |

The change:

- Removes the false "Fault" alarm on Plus installs (the user-
  visible bug).
- Does not introduce a false "OK" claim for an integer we haven't
  independently verified as healthy.
- Leaves room to add an explicit "Fault" mapping the day someone
  posts a confirmed contactor-weld report with a specific integer
  we can attribute.

## Carries forward from rc24

- All rc24 fixes (OTA panic on raw POST body, browser FormData
  wrap, Content-Type guard)
- rc23 fresh-install overlay gate
- rc22 MQTT diagnostic surface + HA discovery
- rc21 reentrancy fix, OTA-during-upload panic fix, boot history,
  XSS, token bucket, universal retry, /settings truncation, PIN
  dual-fact, Auto Lock dual-shape, WiFi enrichment, stress + probe
  scripts.

## Upgrading

- **From rc24** — OTA via `/ota` is safe. The grounding display
  updates as soon as the gateway's next BLE read of `r_wel`
  completes (~30 s after BLE reconnects post-reboot).
- **From earlier** — same as rc24's upgrade path (USB-flash is the
  reliable route from rc14 or earlier; OTA is fine from rc24 or
  later).

## SHA256

See `SHA256SUMS.txt`.
