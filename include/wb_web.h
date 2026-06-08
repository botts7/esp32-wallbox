#pragma once

#include <Arduino.h>

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
