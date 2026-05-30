# v2.4.0-rc23

**Release-blocker fix for fresh installers of rc21 and rc22.**

peter-mcc reported on issue #4 that a USB-flash of rc21 onto a fresh
ESP32-S3 board rendered the AP-mode setup page as a stuck dark
"Loading…" screen — the user couldn't reach the config form to type
in WiFi credentials. The same failure shape hit on an OTA from rc14
onto a board that was configured for WiFi but had no charger paired.

## Root cause

The rc21 boot overlay server-renders with the `show` class whenever
the current BLE state is not "connected", and is designed to be
dismissed by a WebSocket `ble` push with `state === 'connected'`.

In setup mode there is **no BLE task running** — either because AP
mode hasn't started one yet, or because no charger address is
configured. Without a BLE task there is no `ble` event ever pushed
over the WebSocket, so the overlay never has anything to react to.
The page sits showing the overlay forever.

## Fix

`htmlHead()` no longer emits the boot-overlay HTML at all when the
gateway is in setup mode. The setup mode check is:

```cpp
bool inSetupMode = webServer.isAPMode() || !configMgr.hasBLE();
```

i.e. either the captive portal is active, or no `bleAddr` is
configured. The JS handlers for the overlay were already null-safe
(`if(O) ...` everywhere), so with no overlay element they cleanly
no-op — including the WS-drop watchdog and the nav-click "Loading…"
handler, which would otherwise have re-shown the overlay even though
it was missing on the server side.

`WBWebServer` gains a public `isAPMode()` getter so `htmlHead()` can
ask. No behaviour change for an operational gateway (BLE configured
and connected) — the overlay still renders and behaves exactly as it
does on rc21/rc22.

## Carries forward from rc22

- All rc21 fixes (reentrancy panic, OTA-during-upload panic, boot
  history, XSS, overlay UX, token bucket, universal retry, /settings
  truncation, PIN dual-fact, Auto Lock dual-shape, WiFi enrichment,
  stress + probe scripts).
- rc22's MQTT diagnostic surface and Home Assistant auto-discovery
  for the rc21 observability fields (`max_reentry`, `tokens`,
  `boot_reason`, `heap_min_ever`, `ble_paused`, etc.).

## Upgrading

If you're on rc21 or rc22 with a working configured gateway: **OTA
via `/ota` is safe** — the setup-mode gate only changes behaviour
during AP mode and unconfigured-BLE startup. Your operational
gateway never enters either state at runtime.

If you tried to flash rc21 or rc22 onto a fresh board and got stuck
on the loading screen: **USB-flash rc23 instead.** The captive
portal will now render normally and let you set up WiFi + BLE.

Existing rc14 (or earlier) users who haven't upgraded yet: USB-flash
rc23 directly — the OTA-from-rc14 path has a known bug where ~50%
of attempts panic during upload (rc14's reentrancy bug, not rc23's
fault; documented in #4). USB-flash is the reliable path.

## SHA256

See `SHA256SUMS.txt`.
