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
    _cfg.reminderLeadMin = _prefs.getULong("rem_lead", 10);
    _cfg.mainsVoltage = _prefs.getULong("mains_v", 230);
    _cfg.controlOwner = _prefs.getString("ctrl_owner", "wallbox_schedule");
    _cfg.haDiscoveryPrefix = _prefs.getString("ha_prefix", "homeassistant");
    _cfg.haDeviceId   = _prefs.getString("ha_devid", "wallbox_pulsar_max");
    _cfg.haDiscoveryEnabled = _prefs.getBool("ha_disc", true);
    _cfg.lastSeenFw   = _prefs.getString("last_fw", "");

    // FORENSIC (task #97): also log the LENGTHS so trailing whitespace,
    // hidden control characters, or empty-but-set values surface in the
    // boot trace. Quote strings so we can see whitespace at the edges.
    Log.println("[Config] Loaded from NVS:");
    Log.printf("  WiFi SSID:  '%s' (len=%u)\n",
        _cfg.wifiSSID.c_str(), _cfg.wifiSSID.length());
    Log.printf("  WiFi pass:  len=%u (masked)\n", _cfg.wifiPass.length());
    Log.printf("  MQTT host:  '%s' (len=%u) port=%u\n",
        _cfg.mqttHost.c_str(), _cfg.mqttHost.length(), _cfg.mqttPort);
    Log.printf("  MQTT user:  '%s' (len=%u)\n",
        _cfg.mqttUser.c_str(), _cfg.mqttUser.length());
    Log.printf("  MQTT pass:  len=%u (masked)\n", _cfg.mqttPass.length());
    Log.printf("  MQTT cid:   '%s'\n", _cfg.mqttClientId.c_str());
    Log.printf("  BLE addr:   '%s' (len=%u) pin=%s\n",
        _cfg.bleAddr.c_str(), _cfg.bleAddr.length(),
        _cfg.blePin.length() > 0 ? "set" : "none");
    Log.printf("  BLE svc:    '%s' (len=%u)\n",
        _cfg.bleService.c_str(), _cfg.bleService.length());
    Log.printf("  BLE chr:    '%s' (len=%u)\n",
        _cfg.bleChar.c_str(), _cfg.bleChar.length());
    Log.printf("  BLE txchr:  '%s' (len=%u)\n",
        _cfg.bleTxChar.c_str(), _cfg.bleTxChar.length());
    Log.printf("  chg_model:  '%s'\n", _cfg.chargerModel.c_str());
    Log.printf("  auth:       en=%d user='%s' pass_len=%u\n",
        (int)_cfg.authEnabled, _cfg.authUser.c_str(), _cfg.authPass.length());
    Log.printf("  polls:      status=%lums realtime=%lums\n",
        (unsigned long)_cfg.statusPollMs, (unsigned long)_cfg.realtimePollMs);
    Log.printf("  reminder:   lead=%lumin\n", (unsigned long)_cfg.reminderLeadMin);
    Log.printf("  mains_v:    %luV (phase-current power fallback)\n", (unsigned long)_cfg.mainsVoltage);
    Log.printf("  HA:         prefix='%s' devid='%s' discovery=%s\n",
        _cfg.haDiscoveryPrefix.c_str(), _cfg.haDeviceId.c_str(),
        _cfg.haDiscoveryEnabled ? "on" : "off");
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
    _prefs.putULong("rem_lead", _cfg.reminderLeadMin);
    _prefs.putULong("mains_v", _cfg.mainsVoltage);
    _prefs.putString("ctrl_owner", _cfg.controlOwner);
    _prefs.putString("ha_prefix", _cfg.haDiscoveryPrefix);
    _prefs.putString("ha_devid", _cfg.haDeviceId);
    _prefs.putBool("ha_disc", _cfg.haDiscoveryEnabled);
    _prefs.putString("last_fw", _cfg.lastSeenFw);
    Log.println("[Config] Saved to NVS");
}

void ConfigManager::reset() {
    _prefs.clear();
    _cfg = WBConfig{};
    Log.println("[Config] Reset to defaults");
    // Also wipe the failure-history namespace so a freshly-onboarded
    // gateway doesn't carry forward a stale "last attempt failed"
    // banner from before the reset.
    Preferences p;
    if (p.begin("wbfail", false)) { p.clear(); p.end(); }
}

void ConfigManager::recordWifiFail(uint8_t reason, const String& ssid) {
    Preferences p;
    if (!p.begin("wbfail", false)) return;
    p.putUChar("reason", reason);
    p.putString("ssid", ssid);
    p.end();
    Log.printf("[Config] Recorded WiFi-join failure: reason=%u ssid='%s'\n",
        (unsigned)reason, ssid.c_str());
}

void ConfigManager::clearWifiFail() {
    Preferences p;
    if (!p.begin("wbfail", false)) return;
    if (p.isKey("reason")) p.remove("reason");
    if (p.isKey("ssid"))   p.remove("ssid");
    p.end();
}

uint8_t ConfigManager::lastWifiFailReason() const {
    Preferences p;
    if (!p.begin("wbfail", true)) return 0;
    uint8_t r = p.getUChar("reason", 0);
    p.end();
    return r;
}

String ConfigManager::lastWifiFailSsid() const {
    Preferences p;
    if (!p.begin("wbfail", true)) return "";
    String s = p.getString("ssid", "");
    p.end();
    return s;
}
