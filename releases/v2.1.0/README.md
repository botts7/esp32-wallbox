# Wallbox Gateway v2.1.0 — Pre-built Firmware

## What's new in v2.1.0

- Sessions page redesign with stats tiles (All-time / This Week / This Month) and localStorage caching for instant revisits.
- Daily Charging view groups sub-sessions by day; click any day to expand individual sessions.
- "Load older sessions" pagination through full charger history.
- Schedule add / edit / delete with proper sid management.
- Phase Switch and Halo LED settings panels (matching the official app's Standby toggle).
- Eco Smart panel pre-fills current state and corrects mode 1/2 mapping.
- "Release BLE for App" button — temporarily disconnect so the official Wallbox app can pair.
- FOUC fix, faster page navigation (CSS in `<head>`), heatmap distributes kWh across hours instead of dumping into start hour.

See [CHANGELOG.md](../../CHANGELOG.md) for the full list.

## Files

| File | Size | Purpose |
|------|------|---------|
| `wallbox-gateway-esp32s3-v2.1.0.bin` | ~1.1 MB | Main application firmware |
| `bootloader.bin` | 15 KB | ESP32 bootloader |
| `partitions.bin` | 3 KB | OTA partition table |

## Target Hardware

- **ESP32-S3** (tested on ESP32-S3-WROOM-1U-N16R8 with IPEX antenna)
- Should also work on ESP32-S3-DevKitC-1
- Requires **8MB+ flash** (N8R2, N16R8 etc.)

## Installation

### Option 1: OTA update (recommended if you have v2.0.x running)

1. Open `http://wallbox-gw.local/ota` (or the gateway's IP)
2. Upload `wallbox-gateway-esp32s3-v2.1.0.bin`
3. Wait for progress bar to complete
4. Device reboots automatically

### Option 2: ESP Web Tools (in-browser, fresh install)

1. Open https://esp.huhn.me/ in Chrome or Edge
2. Click **Connect** → select your ESP32's COM port
3. Click **Erase** to clear flash (important on first install)
4. Click **Program** and upload these files:
   - `bootloader.bin` → offset `0x0`
   - `partitions.bin` → offset `0x8000`
   - `wallbox-gateway-esp32s3-v2.1.0.bin` → offset `0x10000`
5. Click **Reset** to boot

### Option 3: esptool.py command line

```bash
pip install esptool

esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash \
  0x0 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 wallbox-gateway-esp32s3-v2.1.0.bin
```

## First Boot (fresh install only)

After flashing, the ESP32 starts in **AP mode**:

1. Connect to WiFi: **`WallboxGW-Setup`** (password: `wallbox123`)
2. Open `http://192.168.4.1/`
3. Configure WiFi, MQTT broker, and BLE address
4. Save & Reboot — gateway appears as `wallbox-gw.local`

## Notes

- HA users updating from v2.0.x: the lifetime energy sensor is now properly configured for the **Energy dashboard**. You may need to delete the cached "Wallbox Pulsar MAX" device once in HA → Settings → Devices to pick up the fixed metadata.
- Eco Smart mode mapping changed: `mode=1` is now Full Green, `mode=2` is Solar+Grid (was inverted in v2.0.x). If you had automations sending mode values, swap 1 and 2.

## SHA256 Checksums

```
360b4435824cfff9091b20c11013ba0d4d662d06693edc05edfb79a0d636f487  wallbox-gateway-esp32s3-v2.1.0.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```

Verify with `sha256sum *.bin` (Linux/Mac) or PowerShell `Get-FileHash`.
