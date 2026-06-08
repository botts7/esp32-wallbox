# v2.4.1

First non-RC release on the `v2.4-pulsar-plus` branch. Adds five new
charger-side BAPI exposures with Home Assistant discovery, hardens the
OTA upload path against the failure modes seen during the rc23–rc25
cycle, and introduces a runtime tripwire that catches main-loop wedges
of the kind suspected behind the original peter-mcc #4 report.

## What's new

### Charger-side BAPI exposures

Five new read-only BAPI methods, surfaced on `/info`, `/api/status`,
and as Home Assistant entities:

- **`fw_v_`** — charger app firmware string (`6.11.16` on the MAX
  tested), project identifier (`prj15-pulsar-max`), and a derived
  friendly model name (Pulsar MAX / Plus / Copper SB / Quasar /
  Quasar 2). Previously only the BLE module's firmware was visible,
  which testers routinely confused for the charger app version.
- **`r_ses`** — lifetime charging-session counter. Surfaces as a
  `total_increasing` HA sensor and as a badge on the Sessions page.
- **`r_hsh`** — Power Boost (ICP) current cap.
- **`r_lck`** — discrete lock state, published as a `device_class:
  lock` binary_sensor with the inversion correctly handled (HA's
  lock class treats `on` as unlocked).
- **`gnsta`** — charger-side WiFi link details (SSID, IP, signal
  quality %). The charger has its own WiFi separate from the
  gateway's WiFi; both are now visible.

### OTA hardening

- **503 + Retry-After on admission rejection.** The upload path no
  longer fails with a generic 500 when the admission gate denies
  the upload — it emits a proper 503 with `Retry-After` indicating
  when the gate will open.
- **Browser auto-retry on 503.** `/ota`'s upload script parses
  Retry-After, shows a live countdown, and re-fires the upload
  once. Eliminates the manual re-click that previously followed
  every just-after-reboot OTA attempt.
- **Relaxed admission window after first successful OTA.** A device
  that has completed one OTA + reached healthy state flips a
  NVS-backed `ota_proven` flag. Subsequent admissions drop the
  uptime threshold from 60 s to 15 s.
- **RAII watchdog scope (`wb_watchdog`).** Backport of the
  ESPHome PR #16138 pattern. Guarantees the Task WDT goes back to
  its default 5 s timeout after the OTA path finishes, regardless
  of which exit branch fires. Previously a failed OTA could leave
  the WDT relaxed for the rest of the device's uptime.
- **Optional MD5 verification.** When the client sends an
  `X-Firmware-MD5` header (curl `--header`, HA OTA scripts), the
  Update library streams the hash alongside the partition writes
  and refuses to commit on mismatch. Browsers continue to skip
  this to avoid the multi-second hash step on a 1.5 MB file.

### Loop-wedge tripwire

A new `loop_max_ms` metric tracks the longest gap between
consecutive main-loop iterations. Surfaced on `/api/health`, as an
HA sensor, and in a new Runtime Health section in the Diagnostics
card on `/info`. The first 60 s of uptime are excluded — boot-phase
MQTT discovery publishes saturate the metric for several seconds
and aren't the kind of wedge the tripwire is meant to detect.

Healthy steady-state values are under ~500 ms. Anything multi-second
indicates a wedge of the kind that motivated the rc15 BLE-task
refactor.

### Pre-release test gates

`RELEASING.md` documents the four gates that any tagged non-RC
release must pass — fresh-install smoke, browser OTA, stress +
tripwires, and a read-only BAPI probe. These were introduced after
the rc23/rc24/rc25 cycle landed regressions that passed the
maintainer's curl-based testing but broke under tester browser
testing.

## Live-validation

Tested end-to-end on a Pulsar MAX with all gates green:

- All five BAPI exposures returned data on first read.
- Two consecutive OTAs confirmed `ota_proven` flips from false → true
  and the relaxed 15 s window engages.
- `loop_max_ms` settled to ~400 ms post-boot after the cutoff fix.
- Lock state renders correctly across the inversion.
- WiFi signal renders as a percentage with the right field name.

## Migration notes

Anyone who picked up a pre-release dev build with `chg_net_rssi` HA
entity will find that entity stale after upgrading — the rename to
`chg_net_signal` is permanent. Delete the old entity manually in HA:
Settings → Devices → Wallbox device → 3-dot menu on the stale RSSI
sensor → Delete.

## Flashing

USB:
```bash
python -m platformio run --target upload
```

OTA (gateway IP at `<gw>`, OTA password from setup):
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```
