# Wallbox Gateway v2.0.1 — Pre-built Firmware

## Files

| File | Size | Purpose |
|------|------|---------|
| `wallbox-gateway-esp32s3-v2.0.1.bin` | ~1.1 MB | Main application firmware |
| `bootloader.bin` | 15 KB | ESP32 bootloader |
| `partitions.bin` | 3 KB | OTA partition table |

## Target Hardware

- **ESP32-S3** (tested on ESP32-S3-WROOM-1U-N16R8 with IPEX antenna)
- Should also work on ESP32-S3-DevKitC-1
- Requires **8MB+ flash** (N8R2, N16R8 etc.)

## Installation

### Option 1: ESP Web Tools (easiest, in-browser)

1. Open https://esp.huhn.me/ in Chrome or Edge
2. Click **Connect** → select your ESP32's COM port
3. Click **Erase** to clear flash (important on first install)
4. Click **Program** and upload these files:
   - `bootloader.bin` → offset `0x0`
   - `partitions.bin` → offset `0x8000`
   - `wallbox-gateway-esp32s3-v2.0.1.bin` → offset `0x10000`
5. Click **Reset** to boot

### Option 2: esptool.py command line

```bash
pip install esptool

# All three files in one command:
esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash \
  0x0 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 wallbox-gateway-esp32s3-v2.0.1.bin

# Or just the application (if bootloader already flashed):
esptool.py --chip esp32s3 --port COM4 --baud 921600 write_flash \
  0x10000 wallbox-gateway-esp32s3-v2.0.1.bin
```

### Option 3: OTA update (if you already have a previous version running)

1. Open `http://wallbox-gw.local/ota` (or the gateway's IP)
2. Upload `wallbox-gateway-esp32s3-v2.0.1.bin`
3. Wait for progress bar to complete
4. Device reboots automatically

## First Boot

After flashing, the ESP32 starts in **AP mode**:

1. On your phone or computer, connect to WiFi: **`WallboxGW-Setup`** (password: `wallbox123`)
2. Open `http://192.168.4.1/` in your browser
3. Configure:
   - WiFi SSID + password (tap Scan to find your network)
   - MQTT broker (usually your Home Assistant IP, port 1883)
   - BLE address (tap Scan to find your Wallbox — it should show up as `WB1234567`)
4. Save & Reboot
5. The gateway will connect to your WiFi and appear as `wallbox-gw.local`

## Verification

- Web UI: `http://wallbox-gw.local/` → should show the dashboard with BLE status
- Home Assistant: Settings → Devices → should auto-discover "Wallbox Pulsar MAX"
- Serial logs (USB): `115200` baud → shows boot + BLE activity

## Troubleshooting

See the main [README.md](../../README.md#troubleshooting) for detailed troubleshooting.

## Changelog

See [CHANGELOG.md](../../CHANGELOG.md) for the full history.

## SHA256 Checksums

Run `sha256sum *.bin` after download to verify files match the release.
