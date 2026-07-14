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
    float t = temperatureRead();
    // temperatureRead() can transiently return NaN/inf; String(NaN,1) serialises
    // to "nan", which is INVALID JSON and breaks the whole gateway payload for HA
    // ("Invalid state message ''"). Emit null on a bad read instead.
    if (isnan(t) || isinf(t)) return String("null");
    return String(t, 1);
#else
    return String("null");
#endif
}

// Escape a device-reported string for safe interpolation into the hand-built
// gateway / status JSON (these use += concatenation, not a serializer). A stray
// quote/backslash or control char — a partial BLE read, or an odd WiFi SSID —
// would otherwise produce invalid JSON that HA can't parse. Quotes/backslashes
// are escaped; control chars are dropped.
inline String wb_jsonEsc(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c >= 0x20)  out += c;   // drop control chars (< 0x20)
    }
    return out;
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
