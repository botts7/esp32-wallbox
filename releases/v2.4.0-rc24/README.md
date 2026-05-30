# v2.4.0-rc24

**Release-blocker fix for browser-driven OTA on rc21, rc22, and rc23.**

peter-mcc captured a serial-port panic backtrace in #4 that decoded
in seconds and exposed a bug that has been bricking every
browser-driven OTA since rc21.

## Root cause

Our `/ota` web UI's `doOTA()` was calling `xhr.send(File)` — passing
the File object directly to the XHR. Browsers, when given a raw
File, send the bytes as a raw body with `Content-Type` derived from
the file's MIME (`application/octet-stream` for a `.bin`). The body
is **not** wrapped in `multipart/form-data`.

The Arduino WebServer dispatches incoming POSTs into two paths
depending on the body shape: `upload()` for multipart, `raw()` for
everything else. Both paths call the same `_ufn` lambda we register
for `/api/ota`. But `_currentUpload` (the `HTTPUpload` struct our
handler reads via `http.upload()`) is **only allocated on the
multipart path**. On the raw path it's a null `unique_ptr`.

Our handler's first instruction was `HTTPUpload& upload = http.upload();`
which dereferences `*_currentUpload` — null pointer dereference,
`EXCVADDR = 0x00000000`, `LoadProhibited` panic, instant reboot.

Why we missed this in pre-release testing: my OTA test harness used
`curl -F firmware=@file.bin` which **is** multipart, so it hit the
working path. The browser web-UI path was never exercised by my
tests. Peter using the actual `/ota` page in his browser hit it
every single time.

## Fix (two layers)

**Layer 1 — client.** `doOTA()` now wraps the file in `FormData` and
sends that:

```js
var fd = new FormData();
fd.append('firmware', f);
xhr.send(fd);  // was xhr.send(f) — sent raw body
```

Browser sends `multipart/form-data; boundary=...`, WebServer
dispatches via `upload()`, `_currentUpload` is allocated, handler
runs as designed. This alone covers our own web UI.

**Layer 2 — server.** `handleOtaUpload()` now validates
`Content-Type` contains `multipart/` **before** touching
`http.upload()`. Missing header, wrong type, unparseable — all
reject cleanly. Required adding `Content-Type` to
`http.collectHeaders(...)` because Arduino WebServer drops every
header not in that list.

Both layers ship in rc24. Layer 1 fixes the *known* browser failure;
Layer 2 catches any *other* tool (Home Assistant, curl
`--data-binary`, custom OTA scripts) that might POST a raw body.

## Verification

On the maintainer's gateway, every variant tested:

| Pattern | Expected | Actual |
|---------|----------|--------|
| Multipart OTA (`curl -F`) | Completes, intentional reboot | ✓ PASS |
| Raw octet-stream POST | Rejected, no crash | ✓ PASS |
| No Content-Type header | Rejected, no crash | ✓ PASS |
| `application/x-www-form-urlencoded` | Rejected, no crash | ✓ PASS |
| Empty POST body | Rejected, no crash | ✓ PASS |
| Multipart again (regression check) | Completes | ✓ PASS |

`max_reentry` stayed at 1, heap stable at 142 KB, zero unexpected
panics in boot history after the Layer 2 fix landed.

## Upgrading

- **From rc21 / rc22 / rc23**: OTA via `/ota` is now SAFE — the
  rc24 web UI sends multipart, so the upload completes cleanly.
  If you're currently on one of those versions and the OTA button
  has been crashing your board, this release is the fix.
- **From rc14 or earlier**: USB-flash is still the reliable path,
  same as it has been.
- **Fresh install**: USB-flash rc24 directly.

## Carries forward from rc23

- Boot overlay setup-mode gate (fixed dark "Loading..." screen for
  fresh installs)
- All rc22 MQTT diagnostic surface + HA discovery
- All rc21 fixes (reentrancy panic, OTA-during-upload panic, boot
  history, XSS, token bucket, universal retry, /settings truncation,
  PIN dual-fact, Auto Lock dual-shape, WiFi enrichment, stress +
  probe scripts).

## SHA256

See `SHA256SUMS.txt`.
