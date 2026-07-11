#pragma once

#include <Arduino.h>

// Gateway die-temperature as a JSON value. ESP32-S3/S2/C3/C6 have a usable
// internal temperature sensor; the classic ESP32 (WROOM — future esp32dev
// target, #154) does NOT — temperatureRead() there returns a fixed/garbage value
// (~53.3 °C). So emit the real reading only where it's meaningful, else JSON
// `null` so HA shows the "Gateway Temperature" sensor as unavailable instead of a
// misleading number. Shared by /api/status (wb_web.cpp) and the MQTT gateway
// payload (main.cpp) so both stay consistent.
inline String wb_chipTempJson() {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \
    defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
    return String(temperatureRead(), 1);
#else
    return String("null");
#endif
}

// Web server for configuration UI.
// In AP mode: captive portal for initial WiFi/MQTT/BLE setup.
// In STA mode: config page accessible at device IP.

class WBWebServer {
public:
    // Start in AP mode (captive portal) — no WiFi config yet
    void beginAP();

    // Start in STA mode — config page on local network
    void beginSTA();

    // Call from main loop
    void loop();

    // Update cached charger data (called from main loop after BLE poll)
    void updateCache(const String& status, const String& realtime);

    // Check if user submitted config and wants to reboot
    bool shouldReboot() const { return _rebootRequested; }

    void requestReboot() { _rebootRequested = true; }

    // Lets htmlHead() decide whether to render the boot overlay. In AP /
    // setup mode there is no BLE link to wait for, so the overlay would
    // never dismiss and the setup page would appear stuck. peter-mcc hit
    // exactly this on a fresh USB-flash of rc21 — see issue #4.
    bool isAPMode() const { return _apMode; }

private:
    bool _rebootRequested = false;
    bool _apMode = false;
};

extern WBWebServer webServer;
