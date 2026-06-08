# Releasing

These gates exist because the rc23 / rc24 / rc25 cycle shipped fixes
that passed the maintainer's headless tests but broke when peter-mcc
exercised them through a browser. The lesson: a curl-based test surface
is not the same as the user-facing test surface. Don't skip these.

A release candidate (`-rcN`) does not need every gate, but **a tagged
non-RC release MUST pass all four below.**

## Gate 1 — Fresh-install smoke

Goal: confirm the build still works on a board that has never seen
this firmware before.

1. Erase flash: `python -m platformio run -e esp32s3 --target erase`
2. USB-flash this build: `python -m platformio run -e esp32s3 --target upload`
3. Open serial monitor, watch for the boot banner + setup-mode AP.
4. Phone connects to `WallboxGW-Setup`, captive portal loads.
5. Submit Wi-Fi creds. Board reboots into STA mode and lands on
   the dashboard.
6. No black screen at any point. No serial-monitor panics.

A failure here is **always blocking** — even if the rest of the
gateway works, anyone with a brand-new board would never get past it.
This is the rc23 regression class.

## Gate 2 — Browser OTA

Goal: confirm the OTA upload path works the way a normal user invokes
it (not the way the maintainer invokes it from curl).

1. From `/info` (logged in), click **Firmware Update**.
2. Choose a `.bin` from `.pio/build/esp32s3/firmware.bin`.
3. Click **Upload Firmware**. The progress bar must reach 100%.
4. Status flips to "Update complete! Rebooting..." and the device
   comes back on the same Wi-Fi.
5. `/info` footer shows the new version string.

If admission rejects (gateway just rebooted), the new auto-retry
should kick in and complete on the second try without the user
re-clicking. That's the rc24 regression class.

## Gate 3 — Stress + tripwires

Goal: confirm no runtime wedge regressions. Run the gateway for
**at least 10 minutes** under normal conditions, then check
`/api/health`.

```bash
curl -s http://<gateway-ip>/api/health | python -m json.tool
```

Required:

- `"max_reentry": 1` — anything > 1 means a re-entrant
  `handleClient()` call slipped through. Hard blocker.
- `"loop_max_ms": < 2000` — anything ≥ 2000 ms is a wedge
  observation. Investigate before tagging.
- `"mqtt_reconnects": 0` (from `/api/diag/disconnects`)
- `"ble_reconnects": 0` over the same window (steady BLE)
- `"heap_free": > 80000` — sub-80 KB suggests a leak.

## Gate 4 — Read-only BAPI probe

Goal: confirm the new exposures we add for a release actually return
values on a real charger, not just compile cleanly.

For each new BAPI method this release introduces, hit it via
`/api/command` with the read-only verb and confirm a structured
response. Don't push without seeing real data echoed back.

## Tagging

Only when all four gates are green:

1. Bump `WB_VERSION` in `include/wb_version.h` (or rely on
   `git describe --tags --dirty` for an rc).
2. `git commit -am "Release vX.Y.Z"`
3. `git tag -a vX.Y.Z -m "..."`
4. Stage the binary + a release README in `releases/vX.Y.Z/`.
5. Push the branch and tag together.
6. Create the GitHub Release with the artifact attached.

## Scanning the diff for secrets

Before any `git push`, run:

```bash
git diff origin/main..HEAD | grep -iE 'password|token|api_key|ssid|192\.168|botts|peter'
```

If hits appear, redact before pushing. This list catches the values
that have leaked in past sessions and would expose this maintainer's
or testers' networks.
