#include "wb_config.h"
#include "wb_log.h"

ConfigManager configMgr;

// Default u-blox UUIDs (confirmed on Pulsar MAX)
static const char* DEFAULT_BLE_SVC  = "2456e1b9-26e2-8f83-e744-f34f01e9d701";
static const char* DEFAULT_BLE_CHR  = "2456e1b9-26e2-8f83-e744-f34f01e9d703";

void ConfigManager::begin() {
    _prefs.begin("wbconfig", false);
    load();
}

void ConfigManager::load() {
    _cfg.wifiSSID     = _prefs.getString("wifi_ssid", "");
    _cfg.wifiPass     = _prefs.getString("wifi_pass", "");
    _cfg.mqttHost     = _prefs.getString("mqtt_host", "");
    _cfg.mqttPort     = _prefs.getUShort("mqtt_port", 1883);
    _cfg.mqttUser     = _prefs.getString("mqtt_user", "");
    _cfg.mqttPass     = _prefs.getString("mqtt_pass", "");
    _cfg.mqttClientId = _prefs.getString("mqtt_cid", "wallbox-gw");
    _cfg.bleAddr      = _prefs.getString("ble_addr", "");
    _cfg.blePin       = _prefs.getString("ble_pin", "");
    _cfg.bleService   = _prefs.getString("ble_svc", DEFAULT_BLE_SVC);
    _cfg.bleChar      = _prefs.getString("ble_chr", DEFAULT_BLE_CHR);
    _cfg.bleTxChar    = _prefs.getString("ble_txchr", "");
    _cfg.chargerModel = _prefs.getString("chg_model", "max");
    _cfg.authEnabled  = _prefs.getBool("auth_en", false);
    _cfg.authUser     = _prefs.getString("auth_user", "admin");
    _cfg.authPass     = _prefs.getString("auth_pass", "");
    _cfg.statusPollMs = _prefs.getULong("poll_status", 10000);
    _cfg.realtimePollMs = _prefs.getULong("poll_rt", 30000);
    _cfg.haDiscoveryPrefix = _prefs.getString("ha_prefix", "homeassistant");
    _cfg.haDeviceId   = _prefs.getString("ha_devid", "wallbox_pulsar_max");

    Log.println("[Config] Loaded from NVS:");
    Log.printf("  WiFi: %s\n", _cfg.wifiSSID.c_str());
    Log.printf("  MQTT: %s:%d\n", _cfg.mqttHost.c_str(), _cfg.mqttPort);
    Log.printf("  BLE:  %s (PIN: %s)\n", _cfg.bleAddr.c_str(),
                  _cfg.blePin.length() > 0 ? "set" : "none");
}

void ConfigManager::save() {
    _prefs.putString("wifi_ssid", _cfg.wifiSSID);
    _prefs.putString("wifi_pass", _cfg.wifiPass);
    _prefs.putString("mqtt_host", _cfg.mqttHost);
    _prefs.putUShort("mqtt_port", _cfg.mqttPort);
    _prefs.putString("mqtt_user", _cfg.mqttUser);
    _prefs.putString("mqtt_pass", _cfg.mqttPass);
    _prefs.putString("mqtt_cid", _cfg.mqttClientId);
    _prefs.putString("ble_addr", _cfg.bleAddr);
    _prefs.putString("ble_pin", _cfg.blePin);
    _prefs.putString("ble_svc", _cfg.bleService);
    _prefs.putString("ble_chr", _cfg.bleChar);
    _prefs.putString("ble_txchr", _cfg.bleTxChar);
    _prefs.putString("chg_model", _cfg.chargerModel);
    _prefs.putBool("auth_en", _cfg.authEnabled);
    _prefs.putString("auth_user", _cfg.authUser);
    _prefs.putString("auth_pass", _cfg.authPass);
    _prefs.putULong("poll_status", _cfg.statusPollMs);
    _prefs.putULong("poll_rt", _cfg.realtimePollMs);
    _prefs.putString("ha_prefix", _cfg.haDiscoveryPrefix);
    _prefs.putString("ha_devid", _cfg.haDeviceId);
    Log.println("[Config] Saved to NVS");
}

void ConfigManager::reset() {
    _prefs.clear();
    _cfg = WBConfig{};
    Log.println("[Config] Reset to defaults");
}
