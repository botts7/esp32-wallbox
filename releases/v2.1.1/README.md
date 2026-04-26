# Wallbox Gateway v2.1.1 — Pre-built Firmware

Polish patch on top of [v2.1.0](../v2.1.0/README.md). Same architecture, additional UX features.

## What's new in v2.1.1

- **Light / Dark / Auto theme toggle** (Settings → Charger → 🎨 Theme). Auto follows the OS / browser preference.
- **CSV export** of session history (Sessions page → 📥 Export CSV). Downloads `wallbox-sessions-YYYY-MM-DD.csv` from the cached session log.
- **Clickable heatmap cells** — tap any heatmap cell to see every session that delivered energy in that day×hour, in a blurred-background modal.
- **Backdrop-blur** on the drilldown modal for better contrast.

See [CHANGELOG.md](../../CHANGELOG.md) for the full v2.1 history.

## Files

| File | Size | Purpose |
|------|------|---------|
| `wallbox-gateway-esp32s3-v2.1.1.bin` | ~1.1 MB | Main application firmware |
| `bootloader.bin` | 15 KB | ESP32 bootloader |
| `partitions.bin` | 3 KB | OTA partition table |

## Installation

If you're on **v2.1.0** or **v2.0.x**: just OTA — open `http://wallbox-gw.local/ota` and upload `wallbox-gateway-esp32s3-v2.1.1.bin`.

For fresh installs see the [v2.1.0 README](../v2.1.0/README.md#installation).

## SHA256 Checksums

```
2bebbe583449b11b46c7670479483e0fdba7b39b3ad335932c7004ac499bf825  wallbox-gateway-esp32s3-v2.1.1.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```
