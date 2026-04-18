#pragma once

#include <Arduino.h>
#include <Preferences.h>

// NVS-backed configuration with web UI for setup.
// On first boot (no WiFi saved), starts AP mode with captive portal.
// After config, reboots and connects normally.
// Web UI remains accessible on local network for changes.

struct WBConfig {
    // WiFi
    String wifiSSID;
    String wifiPass;

    // MQTT
    String mqttHost;
    uint16_t mqttPort = 1883;
    String mqttUser;
    String mqttPass;
    String mqttClientId = "wallbox-gw";

    // Wallbox BLE
    String bleAddr;
    String blePin;       // empty = no PIN
    String bleService;   // default u-blox
    String bleChar;      // default u-blox

    // Web auth
    bool authEnabled = false;
    String authUser = "admin";
    String authPass = "";

    // Timings
    uint32_t statusPollMs = 10000;
    uint32_t realtimePollMs = 30000;

    // HA
    String haDiscoveryPrefix = "homeassistant";
    String haDeviceId = "wallbox_pulsar_max";
};

class ConfigManager {
public:
    void begin();

    // Load config from NVS
    void load();

    // Save config to NVS
    void save();

    // Check if WiFi is configured
    bool hasWiFi() const { return _cfg.wifiSSID.length() > 0; }

    // Check if BLE address is configured
    bool hasBLE() const { return _cfg.bleAddr.length() > 0; }

    // Get config (read-only)
    const WBConfig& get() const { return _cfg; }

    // Get mutable config (for web UI updates)
    WBConfig& mut() { return _cfg; }

    // Reset all config to defaults
    void reset();

private:
    Preferences _prefs;
    WBConfig _cfg;
};

extern ConfigManager configMgr;
