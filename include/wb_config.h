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
    String bleService;   // default u-blox (Pulsar MAX)
    String bleChar;      // single-char mode (MAX): both write + notify here
    String bleTxChar;    // dual-char mode (Pulsar Plus etc.): separate notify char
    String chargerModel; // "max" (default) | "plus" — for status code map + UI presets

    // Web auth
    bool authEnabled = false;
    String authUser = "admin";
    String authPass = "";

    // Timings
    uint32_t statusPollMs = 10000;
    uint32_t realtimePollMs = 30000;

    // Charge reminder: how many minutes before a scheduled charge to raise
    // the "plug in" nudge (plug_reminder). 0 disables the feature entirely.
    uint32_t reminderLeadMin = 10;

    // Charge control owner — who may autonomously start/stop charging.
    // Surfaces/controllers read control_owner from /api/status; only the
    // matching one acts. "wallbox_schedule" (default) | "integration" |
    // "addon" | "none". Advisory only — manual commands always work.
    String controlOwner = "wallbox_schedule";

    // HA
    String haDiscoveryPrefix = "homeassistant";
    String haDeviceId = "wallbox_pulsar_max";

    // Last-seen charger firmware string (GATT 0x180A 0x2A26).
    // Used to detect Wallbox-pushed auto-OTAs across our reboots.
    String lastSeenFw;
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

    // True when the configured model uses the Plus-family BLE protocol
    // (dual-char Nordic UART variant, 0-18 status enum, no BAPI PIN).
    // Plus, Copper SB, Quasar / Quasar 2 all live here.
    bool isPlusFamily() const {
        const String& m = _cfg.chargerModel;
        return m == "plus" || m == "copper" || m == "quasar" || m == "quasar2";
    }

    // Friendly product name for the configured model — shown in HA device card
    // and elsewhere in the UI. Returns ("Wallbox <name>", "<name>") pair.
    void productName(const char*& fullName, const char*& shortName) const {
        const String& m = _cfg.chargerModel;
        if      (m == "plus")    { fullName = "Wallbox Pulsar Plus"; shortName = "Pulsar Plus"; }
        else if (m == "copper")  { fullName = "Wallbox Copper SB";   shortName = "Copper SB"; }
        else if (m == "quasar")  { fullName = "Wallbox Quasar";      shortName = "Quasar"; }
        else if (m == "quasar2") { fullName = "Wallbox Quasar 2";    shortName = "Quasar 2"; }
        else                     { fullName = "Wallbox Pulsar MAX";  shortName = "Pulsar MAX"; }
    }

    // Reset all config to defaults
    void reset();

    // ---- Last WiFi-join failure (3.0 onboarding feedback) ----
    //
    // Persisted in its own NVS namespace ("wbfail") so the captive
    // portal /setup page can render a friendly "your last attempt
    // failed because X" banner. recordWifiFail() is called from
    // wb_net::begin() on the failure path; clearWifiFail() runs on
    // the first successful WiFi join after reboot. Reason 0 = no
    // failure recorded.
    void recordWifiFail(uint8_t reason, const String& ssid);
    void clearWifiFail();
    uint8_t lastWifiFailReason() const;
    String  lastWifiFailSsid()   const;

private:
    Preferences _prefs;
    WBConfig _cfg;
};

extern ConfigManager configMgr;
