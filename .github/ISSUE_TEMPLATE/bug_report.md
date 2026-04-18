---
name: Bug report
about: Something isn't working
labels: bug
---

**Describe the bug**
A clear description of what's wrong.

**Expected behavior**
What should happen instead.

**Hardware**
- ESP32 board model (e.g. ESP32-S3-WROOM-1U-N16R8):
- Antenna type (PCB / IPEX external):
- BLE RSSI to charger (shown on Dashboard):

**Charger**
- Model (e.g. Pulsar MAX, Commander 2):
- Firmware version (shown in Wallbox app or Info page):
- BLE radio (u-blox, Zentri, BGX):

**Gateway version**
- Firmware version (shown in Info page):
- Install method (built from source / pre-built binary / OTA):

**Serial log**
Paste relevant serial output (115200 baud):
```
[paste here]
```

**Network**
- Home Assistant MQTT broker version:
- Router make/model (some routers block mDNS or multicast):
- Can you reach `http://<gateway-ip>/`? (YES / NO)
- Can you reach `http://wallbox-gw.local/`? (YES / NO)

**Additional context**
Anything else that might help.
