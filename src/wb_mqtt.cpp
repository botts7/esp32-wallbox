#include "wb_mqtt.h"
#include "wb_ble.h"
#include "wb_config.h"
#include "wb_log.h"
#include "wb_diag.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// Topic helpers using NVS config
static String baseTopic()      { return "wallbox"; }
static String statusTopic()    { return baseTopic() + "/status"; }
static String realtimeTopic()  { return baseTopic() + "/realtime"; }
static String availTopic()     { return baseTopic() + "/availability"; }
static String cmdPrefix()      { return baseTopic() + "/cmd/"; }

WallboxMQTT wallboxMQTT;
static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

// ---- HA Discovery helpers ----

static void publishDiscoveryEntity(PubSubClient& mqtt, const char* component,
    const char* objectId, const char* name, const char* icon,
    const char* stateTopic, const char* valTemplate,
    const char* unit = nullptr, const char* devClass = nullptr,
    const char* cmdTopic = nullptr, const char* stateClass = nullptr) {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/" + component + "/" + cfg.haDeviceId + "/" + objectId + "/config";

    JsonDocument doc;
    doc["name"] = name;
    doc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    doc["object_id"] = cfg.haDeviceId + "_" + objectId;
    doc["state_topic"] = stateTopic;
    doc["value_template"] = valTemplate;
    doc["availability_topic"] = availTopic();
    if (icon) doc["icon"] = icon;
    if (unit) doc["unit_of_measurement"] = unit;
    if (devClass) doc["device_class"] = devClass;
    if (stateClass) doc["state_class"] = stateClass;
    if (cmdTopic) doc["command_topic"] = cmdTopic;

    // Device block
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = configMgr.get().haDeviceId;
    {
        const char* _fullName = nullptr; const char* _shortName = nullptr;
        configMgr.productName(_fullName, _shortName);
        dev["name"] = _fullName;
        dev["manufacturer"] = "Wallbox";
        dev["model"] = _shortName;
    }
    dev["sw_version"] = "6.11.16";
    // No via_device — the ESP32 gateway IS the device

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

static void publishDiscoverySwitch(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    const char* payloadOn = "1", const char* payloadOff = "0") {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/switch/" + cfg.haDeviceId + "/" + objectId + "/config";

    JsonDocument doc;
    doc["name"] = name;
    doc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    doc["object_id"] = cfg.haDeviceId + "_" + objectId;
    doc["command_topic"] = cmdTopic;
    doc["state_topic"] = stateTopic;
    doc["value_template"] = valTemplate;
    doc["payload_on"] = payloadOn;
    doc["payload_off"] = payloadOff;
    doc["availability_topic"] = availTopic();
    if (icon) doc["icon"] = icon;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = configMgr.get().haDeviceId;
    {
        const char* _fullName = nullptr; const char* _shortName = nullptr;
        configMgr.productName(_fullName, _shortName);
        dev["name"] = _fullName;
        dev["manufacturer"] = "Wallbox";
        dev["model"] = _shortName;
    }

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

static void publishDiscoveryNumber(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    float minVal, float maxVal, float step, const char* unit = nullptr) {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/number/" + cfg.haDeviceId + "/" + objectId + "/config";

    JsonDocument doc;
    doc["name"] = name;
    doc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    doc["object_id"] = cfg.haDeviceId + "_" + objectId;
    doc["command_topic"] = cmdTopic;
    doc["state_topic"] = stateTopic;
    doc["value_template"] = valTemplate;
    doc["min"] = minVal;
    doc["max"] = maxVal;
    doc["step"] = step;
    doc["availability_topic"] = availTopic();
    if (icon) doc["icon"] = icon;
    if (unit) doc["unit_of_measurement"] = unit;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = configMgr.get().haDeviceId;
    {
        const char* _fullName = nullptr; const char* _shortName = nullptr;
        configMgr.productName(_fullName, _shortName);
        dev["name"] = _fullName;
        dev["manufacturer"] = "Wallbox";
        dev["model"] = _shortName;
    }

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

static void publishDiscoveryButton(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* payload_press = "1") {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/button/" + cfg.haDeviceId + "/" + objectId + "/config";

    JsonDocument doc;
    doc["name"] = name;
    doc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    doc["object_id"] = cfg.haDeviceId + "_" + objectId;
    doc["command_topic"] = cmdTopic;
    doc["payload_press"] = payload_press;
    doc["availability_topic"] = availTopic();
    if (icon) doc["icon"] = icon;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = configMgr.get().haDeviceId;
    {
        const char* _fullName = nullptr; const char* _shortName = nullptr;
        configMgr.productName(_fullName, _shortName);
        dev["name"] = _fullName;
        dev["manufacturer"] = "Wallbox";
        dev["model"] = _shortName;
    }

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

// Select entity (dropdown) — for Eco Smart mode, Halo LED, Timezone
static void publishDiscoverySelect(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    const char** options, int optionCount) {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/select/" + cfg.haDeviceId + "/" + objectId + "/config";

    JsonDocument doc;
    doc["name"] = name;
    doc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    doc["object_id"] = cfg.haDeviceId + "_" + objectId;
    doc["command_topic"] = cmdTopic;
    doc["state_topic"] = stateTopic;
    doc["value_template"] = valTemplate;
    doc["availability_topic"] = availTopic();
    if (icon) doc["icon"] = icon;

    JsonArray opts = doc["options"].to<JsonArray>();
    for (int i = 0; i < optionCount; i++) opts.add(options[i]);

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = cfg.haDeviceId;
    {
        const char* _fullName = nullptr; const char* _shortName = nullptr;
        configMgr.productName(_fullName, _shortName);
        dev["name"] = _fullName;
        dev["manufacturer"] = "Wallbox";
        dev["model"] = _shortName;
    }

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

// ---- MQTT Implementation ----

void WallboxMQTT::begin() {
    const WBConfig& cfg = configMgr.get();
    mqttClient.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
    mqttClient.setCallback(_mqttCallback);
    mqttClient.setBufferSize(1024);
    // Widen keepalive from the PubSubClient default (15s) so that
    // brief blockages of MQTT.loop() (e.g. during a chain of BAPI
    // calls in pollSettings) don't trip the client-side timeout.
    // Pair with the MQTT.loop() call in the BLE yield callback to
    // belt-and-braces the keepalive-starvation case.
    mqttClient.setKeepAlive(60);
    _client = &mqttClient;
}

void WallboxMQTT::loop() {
    if (!_client->connected()) {
        // Edge-trigger the disconnect event on the first loop iteration
        // where we notice we're down. _wasConnected guards against
        // double-counting across many reconnect attempts.
        if (_wasConnected) {
            _wasConnected = false;
            wb_diag::reportDisconnect(wb_diag::Kind::MQTT);
        }
        if (millis() - _lastConnectAttempt >= 5000) {
            _connect();
        }
        return;
    }
    if (!_wasConnected) {
        // Just (re)connected — close the open MQTT disconnect event.
        // reportReconnect is a no-op if no disconnect was open (first boot).
        _wasConnected = true;
        wb_diag::reportReconnect(wb_diag::Kind::MQTT);
    }
    _client->loop();
}

bool WallboxMQTT::isConnected() const {
    return _client && _client->connected();
}

void WallboxMQTT::_connect() {
    _lastConnectAttempt = millis();
    Log.println("[MQTT] Connecting...");

    const WBConfig& cfg = configMgr.get();
    const char* user = cfg.mqttUser.length() > 0 ? cfg.mqttUser.c_str() : nullptr;
    const char* pass = cfg.mqttPass.length() > 0 ? cfg.mqttPass.c_str() : nullptr;
    String avail = availTopic();

    // LWT: set availability to offline on disconnect
    if (_client->connect(cfg.mqttClientId.c_str(), user, pass, avail.c_str(), 0, true, "offline")) {
        Log.println("[MQTT] Connected");
        _subscribe();
        publishAvailability(true);
        if (!_discoveryPublished) {
            sendDiscovery();
            _discoveryPublished = true;
        }
    } else {
        Log.printf("[MQTT] Failed, rc=%d\n", _client->state());
    }
}

void WallboxMQTT::_subscribe() {
    String base = baseTopic();
    String cmdWild = cmdPrefix() + "#";
    _client->subscribe(cmdWild.c_str());
    Log.printf("[MQTT] Subscribed to %s\n", cmdWild.c_str());

    _client->subscribe((base + "/config/pin").c_str());
    _client->subscribe((base + "/bapi").c_str());
}

void WallboxMQTT::_mqttCallback(char* topic, byte* payload, unsigned int len) {
    String t = topic;
    String p;
    p.reserve(len);
    for (unsigned int i = 0; i < len; i++) p += (char)payload[i];

    Log.printf("[MQTT] Received: %s = %s\n", topic, p.c_str());

    String prefix = cmdPrefix();
    String base = baseTopic();
    if (t.startsWith(prefix)) {
        String subtopic = t.substring(prefix.length());
        wallboxMQTT._handleCommand(subtopic.c_str(), p.c_str());
    } else if (t == base + "/config/pin") {
        wallboxBLE.setPin(p.c_str());
        Log.printf("[MQTT] PIN updated to: %s\n", p.c_str());
    } else if (t == base + "/bapi") {
        // Raw BAPI command: {"met":"r_dat","par":null}
        JsonDocument doc;
        if (deserializeJson(doc, p) == DeserializationError::Ok) {
            const char* met = doc["met"];
            if (met) {
                String par = "null";
                if (doc.containsKey("par") && !doc["par"].isNull()) {
                    serializeJson(doc["par"], par);
                }
                String resp = wallboxBLE.sendCommand(met, par.c_str());
                if (!resp.isEmpty()) {
                    wallboxMQTT.publishResponse(met, resp);
                }
            }
        }
    }
}

void WallboxMQTT::_handleCommand(const char* subtopic, const char* payload) {
    if (!wallboxBLE.isConnected()) {
        Log.println("[MQTT] BLE not connected, ignoring command");
        return;
    }

    String sub = subtopic;
    String par;

    if (sub == "charging") {
        // payload: "start", "stop", "pause", "resume" or 1/2
        // w_cha par: 1=resume/start, 2=pause/stop (based on Wallbox convention)
        String action = payload;
        action.toLowerCase();
        int val = 0;
        if (action == "start" || action == "resume" || action == "1") val = 1;
        else if (action == "stop" || action == "pause" || action == "2") val = 2;
        else {
            Log.printf("[CMD] Unknown charging action: %s\n", payload);
            return;
        }
        par = String(val);
        wallboxBLE.sendCommand(bapi::MET_START_STOP, par.c_str());

    } else if (sub == "current") {
        // payload: integer 6-32
        int amps = atoi(payload);
        if (amps < 6 || amps > 32) {
            Log.printf("[CMD] Invalid current: %d (must be 6-32)\n", amps);
            return;
        }
        par = String(amps);
        wallboxBLE.sendCommand(bapi::MET_SET_CURRENT, par.c_str());

    } else if (sub == "lock") {
        // payload: "1"/"lock"/"true" or "0"/"unlock"/"false"
        String action = payload;
        action.toLowerCase();
        int val = (action == "1" || action == "lock" || action == "true") ? 1 : 0;
        par = String(val);
        wallboxBLE.sendCommand(bapi::MET_LOCK, par.c_str());

    } else if (sub == "reboot") {
        wallboxBLE.sendCommand(bapi::MET_REBOOT, "null");

    } else if (sub == "autolock") {
        // payload: JSON {"enabled":1,"time":60}
        wallboxBLE.sendCommand(bapi::MET_SET_AUTOLOCK, payload);

    } else if (sub == "eco_smart") {
        wallboxBLE.sendCommand(bapi::MET_SET_ECO_SMART, payload);

    } else if (sub == "schedule") {
        wallboxBLE.sendCommand(bapi::MET_SET_SCHEDULE, payload);

    } else if (sub == "power_boost") {
        wallboxBLE.sendCommand(bapi::MET_SET_POWER_BOOST, payload);

    } else if (sub == "halo") {
        wallboxBLE.sendCommand(bapi::MET_SET_HALO, payload);

    // ---- Native HA entity handlers (Batch 1) ----
    } else if (sub == "autolock_enable") {
        // switch: "1"/"ON" = enable, else disable. Reuses existing time value.
        String s = payload; s.toLowerCase();
        int en = (s == "1" || s == "on" || s == "true") ? 1 : 0;
        String p = "{\"enabled\":" + String(en) + ",\"time\":60}";
        wallboxBLE.sendCommand(bapi::MET_SET_AUTOLOCK, p.c_str());

    } else if (sub == "autolock_time") {
        int secs = atoi(payload);
        if (secs < 10) secs = 60;
        String p = "{\"enabled\":1,\"time\":" + String(secs) + "}";
        wallboxBLE.sendCommand(bapi::MET_SET_AUTOLOCK, p.c_str());

    } else if (sub == "eco_mode") {
        // HA sends: "Off", "Full Green (Solar Only)", "Solar + Grid"
        // BAPI: esm=0 disabled, esm=1 Full Green, esm=2 Solar+Grid
        String s = payload;
        int mode = 0;
        if (s.startsWith("Full Green")) mode = 1;
        else if (s == "Solar + Grid") mode = 2;
        String p = "{\"esm\":" + String(mode) + ",\"ese\":" + String(mode > 0 ? 1 : 0) + ",\"esp\":100}";
        wallboxBLE.sendCommand(bapi::MET_SET_ECO_SMART, p.c_str());

    } else if (sub == "eco_power") {
        int pct = atoi(payload);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        String p = "{\"esp\":" + String(pct) + "}";
        wallboxBLE.sendCommand(bapi::MET_SET_ECO_SMART, p.c_str());

    } else if (sub == "power_sharing") {
        String s = payload; s.toLowerCase();
        int en = (s == "1" || s == "on" || s == "true") ? 1 : 0;
        String p = "{\"dyps\":" + String(en) + "}";
        wallboxBLE.sendCommand(bapi::MET_SET_POWER_SHARE, p.c_str());

    } else if (sub == "phase_switch") {
        String s = payload; s.toLowerCase();
        int en = (s == "1" || s == "on" || s == "true") ? 1 : 0;
        String p = "{\"enabled\":" + String(en) + "}";
        wallboxBLE.sendCommand(bapi::MET_SET_PHASE, p.c_str());

    } else if (sub == "timezone") {
        // HA sends the timezone string directly
        String p = "{\"timezone\":\"" + String(payload) + "\"}";
        wallboxBLE.sendCommand(bapi::MET_SET_TIMEZONE, p.c_str());

    } else {
        Log.printf("[CMD] Unknown command: %s\n", subtopic);
    }
}

void WallboxMQTT::publishStatus(const String& json) {
    if (!isConnected()) return;
    String topic = statusTopic();
    _client->beginPublish(topic.c_str(), json.length(), false);
    _client->print(json);
    _client->endPublish();
}

void WallboxMQTT::publishRealtime(const String& json) {
    if (!isConnected()) return;
    String topic = realtimeTopic();
    _client->beginPublish(topic.c_str(), json.length(), false);
    _client->print(json);
    _client->endPublish();
}

void WallboxMQTT::publishSettings(const String& json) {
    if (!isConnected()) return;
    String topic = baseTopic() + "/settings";
    _client->beginPublish(topic.c_str(), json.length(), true);  // retain
    _client->print(json);
    _client->endPublish();
}

void WallboxMQTT::publishAvailability(bool online) {
    if (!isConnected()) return;
    String topic = availTopic();
    _client->publish(topic.c_str(), online ? "online" : "offline", true);
}

void WallboxMQTT::publishResponse(const char* method, const String& json) {
    if (!isConnected()) return;
    String topic = baseTopic() + "/response/" + method;
    _client->beginPublish(topic.c_str(), json.length(), false);
    _client->print(json);
    _client->endPublish();
}

// ---- HA Auto-Discovery ----

void WallboxMQTT::sendDiscovery() {
    Log.println("[MQTT] Publishing HA discovery...");

    String sTopic = statusTopic();
    String rTopic = realtimeTopic();
    String cPrefix = cmdPrefix();
    String gTopic = baseTopic() + "/response/gateway";
    const char* st = sTopic.c_str();
    const char* rt = rTopic.c_str();

    // Status code map differs between MAX and Plus — Plus uses a clean 0-18
    // enum (per jagheterfredrik/wallbox-ble) while MAX uses sparse hardware
    // codes (161, 178-180, 189-194, 209-210, etc). Plus-family includes
    // Copper SB, Quasar, Quasar 2 — all use the same protocol.
    const bool isPlus = configMgr.isPlusFamily();
    const char* statusMap = isPlus
        ? "{% set m = {0:'Ready',1:'Charging',2:'Waiting for Car',"
          "3:'Waiting for Schedule',4:'Paused',5:'Schedule End',6:'Locked',"
          "7:'Error',8:'Waiting for Current',9:'Power Sharing (Unconfigured)',"
          "10:'Queue (Power Boost)',11:'Discharging',12:'Waiting for MID Auth',"
          "13:'MID Safety Margin',14:'OCPP Unavailable',15:'OCPP Finishing',"
          "16:'OCPP Reserved',17:'Updating',18:'Queue (Eco Smart)'} %}"
        : "{% set m = {0:'Disconnected',1:'Connected',2:'Charging',3:'Paused',"
          "4:'Scheduled',5:'Discharging',6:'Error',7:'Disconnected',8:'Locked',"
          "9:'Updating',14:'Error',16:'Ready',17:'Connected',"
          "18:'Waiting for Schedule',19:'Scheduled',20:'Charging',"
          "21:'Charge Complete',22:'Paused by User',23:'Queue (Power Share)',"
          "24:'Queue (Eco Smart)',25:'Waiting for Schedule',26:'Discharging',"
          "161:'Ready',178:'Paused',179:'Charging',180:'Scheduled',"
          "189:'Ready',193:'Paused',194:'Locked',209:'Reserved (OCPP)',"
          "210:'Updating'} %}";

    // Car-connected codes (anything that's NOT Ready/Locked/Error/Updating/Disconnected)
    const char* carConnectedCodes = isPlus
        ? "[1,2,3,4,5,8,9,10,11,12,13,14,15,16,18]"
        : "[1,2,3,4,5,17,18,19,20,21,22,23,24,25,26,178,179,180,193]";

    // Active-charging codes (CHARGING + DISCHARGING on Plus; the MAX legacy set on MAX)
    const char* chargingCodes = isPlus
        ? "[1,11]"
        : "[2,20,21,179]";

    const char* deviceModelName = nullptr;
    const char* deviceModelShort = nullptr;
    configMgr.productName(deviceModelName, deviceModelShort);

    // Sensors from r_dat (status)
    publishDiscoveryEntity(*_client, "sensor", "charging_power", "Charging Power",
        "mdi:flash", st, "{{ value_json.r.cp | round(2) }}", "kW", "power", nullptr, "measurement");

    // Charging currents are in deciamps (tenths of amps), divide by 10
    publishDiscoveryEntity(*_client, "sensor", "current_l1", "Charging Current L1",
        "mdi:current-ac", st, "{{ (value_json.r.L1 / 10) | round(1) }}", "A", "current", nullptr, "measurement");

    publishDiscoveryEntity(*_client, "sensor", "current_l2", "Charging Current L2",
        "mdi:current-ac", st, "{{ (value_json.r.L2 / 10) | round(1) }}", "A", "current", nullptr, "measurement");

    publishDiscoveryEntity(*_client, "sensor", "current_l3", "Charging Current L3",
        "mdi:current-ac", st, "{{ (value_json.r.L3 / 10) | round(1) }}", "A", "current", nullptr, "measurement");

    // en/gen/grid are in 10-Wh units (centi-kWh), divide by 100 for kWh
    publishDiscoveryEntity(*_client, "sensor", "energy_session", "Session Energy",
        "mdi:lightning-bolt", st, "{{ (value_json.r.en / 100) | round(2) }}", "kWh", "energy", nullptr, "total_increasing");

    publishDiscoveryEntity(*_client, "sensor", "grid_energy", "Grid Energy",
        "mdi:transmission-tower", st, "{{ (value_json.r.grid / 100) | round(2) }}", "kWh", "energy", nullptr, "total_increasing");

    publishDiscoveryEntity(*_client, "sensor", "green_energy", "Green Energy",
        "mdi:leaf", st, "{{ (value_json.r.gen / 100) | round(2) }}", "kWh", "energy", nullptr, "total_increasing");

    // Discharge energy — populates on bidirectional chargers (Quasar 2 V2H)
    // Always 0 on one-way chargers (Pulsar Plus/MAX, Copper, Commander)
    publishDiscoveryEntity(*_client, "sensor", "discharge_energy", "Discharge Energy (V2H)",
        "mdi:battery-arrow-up", st, "{{ (value_json.r.den / 1000) | round(3) }}", "kWh", "energy", nullptr, "total_increasing");

    {
        String tmpl = String("{% set s = value_json.r.st %}") + statusMap + "{{ m.get(s, 'Code ' ~ s) }}";
        publishDiscoveryEntity(*_client, "sensor", "status", "Charger Status",
            "mdi:ev-station", st, tmpl.c_str());
    }

    // Sensors from r_sta (realtime)
    {
        String tmpl = String("{% set s = value_json.r.charger_status %}") + statusMap + "{{ m.get(s, 'Code ' ~ s) }}";
        publishDiscoveryEntity(*_client, "sensor", "charger_status_code", "Status Code",
            "mdi:information", rt, tmpl.c_str());
    }

    publishDiscoveryEntity(*_client, "sensor", "lock_status", "Lock Status",
        "mdi:lock", rt,
        "{% if value_json.r.lock_status == 0 %}Unlocked{% else %}Locked{% endif %}");

    publishDiscoveryEntity(*_client, "sensor", "max_available_current", "Max Available Current",
        "mdi:current-ac", rt, "{{ value_json.r.max_available_current }}", "A");

    publishDiscoveryEntity(*_client, "sensor", "ocpp_status", "OCPP Status",
        "mdi:lan-connect", rt,
        "{% set s = value_json.r.ocpp_status %}"
        "{% if s == 0 %}Not Available{% elif s == 1 %}Not Configured{% elif s == 2 %}Connected{% elif s == 3 %}Charging{% else %}Code {{ s }}{% endif %}");

    // Binary sensor: car connected (charging power > 0 or status indicates connected)
    {
        const WBConfig& bsCfg = configMgr.get();
        String topic = bsCfg.haDiscoveryPrefix + "/binary_sensor/" + bsCfg.haDeviceId + "/car_connected/config";
        JsonDocument doc;
        doc["name"] = "Car Connected";
        doc["unique_id"] = bsCfg.haDeviceId + "_car_connected";
        doc["object_id"] = bsCfg.haDeviceId + "_car_connected";
        doc["state_topic"] = st;
        doc["value_template"] = String("{% if value_json.r.st in ") + carConnectedCodes + " %}ON{% else %}OFF{% endif %}";
        doc["device_class"] = "plug";
        doc["availability_topic"] = availTopic();
        doc["icon"] = "mdi:ev-plug-type2";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = configMgr.get().haDeviceId;
        dev["name"] = deviceModelName;
        dev["manufacturer"] = "Wallbox";
        dev["model"] = deviceModelShort;
        String pl;
        serializeJson(doc, pl);
        _client->beginPublish(topic.c_str(), pl.length(), true);
        _client->print(pl);
        _client->endPublish();
    }

    // Number: max charging current (6-32A)
    String cmdCurrent = cPrefix + "current";
    String cmdCharging = cPrefix + "charging";
    String cmdLock = cPrefix + "lock";
    String cmdReboot = cPrefix + "reboot";

    publishDiscoveryNumber(*_client, "max_charging_current", "Max Charging Current",
        "mdi:current-ac", cmdCurrent.c_str(), st,
        "{{ value_json.r.cur }}", 6, 32, 1, "A");

    // Switch: charging on/off
    {
        String chgTmpl = String("{% if value_json.r.st in ") + chargingCodes + " %}1{% else %}0{% endif %}";
        publishDiscoverySwitch(*_client, "charging", "Charging",
            "mdi:ev-station", cmdCharging.c_str(), st,
            chgTmpl.c_str(), "start", "stop");
    }

    // Switch: lock
    publishDiscoverySwitch(*_client, "lock", "Charger Lock",
        "mdi:lock", cmdLock.c_str(), rt,
        "{% if value_json.r.lock_status == 1 %}1{% else %}0{% endif %}",
        "lock", "unlock");

    // Button: reboot
    publishDiscoveryButton(*_client, "reboot", "Reboot Charger",
        "mdi:restart", cmdReboot.c_str());

    // Sensor: BLE gateway info
    publishDiscoveryEntity(*_client, "sensor", "ble_rssi", "BLE Signal",
        "mdi:bluetooth-connect", gTopic.c_str(),
        "{{ value_json.rssi }}", "dBm", "signal_strength", nullptr, "measurement");

    // Energy meter sensors (from r_dca, published to wallbox/response/meter)
    String mTopic = baseTopic() + "/response/meter";
    const char* mt = mTopic.c_str();

    publishDiscoveryEntity(*_client, "sensor", "mains_voltage", "Mains Voltage",
        "mdi:flash-triangle", mt,
        "{{ value_json.r.v1 }}", "V", "voltage", nullptr, "measurement");

    publishDiscoveryEntity(*_client, "sensor", "grid_power", "House Power",
        "mdi:home-lightning-bolt", mt,
        "{{ value_json.r.p1 }}", "W", "power", nullptr, "measurement");

    publishDiscoveryEntity(*_client, "sensor", "meter_current", "House Current",
        "mdi:current-ac", mt,
        "{{ (value_json.r.c1 / 10) | round(1) }}", "A", "current", nullptr, "measurement");

    publishDiscoveryEntity(*_client, "sensor", "meter_total_energy", "Lifetime Energy",
        "mdi:counter", mt,
        "{{ (value_json.r.e / 1000) | round(1) }}", "kWh", "energy", nullptr, "total_increasing");

    // Charger notifications (from r_not, published every 60s by main.cpp)
    String notifTopic = baseTopic() + "/response/notifications";
    const char* nt = notifTopic.c_str();

    publishDiscoveryEntity(*_client, "sensor", "notification_count", "Active Notifications",
        "mdi:bell-alert-outline", nt,
        "{{ value_json.count }}");

    publishDiscoveryEntity(*_client, "sensor", "notification_latest", "Latest Notification",
        "mdi:bell-outline", nt,
        "{{ value_json.latest or 'None' }}");

    // ========== Settings entities (from wallbox/settings merged topic) ==========
    String setTopic = baseTopic() + "/settings";
    const char* sTopic_ = setTopic.c_str();
    String cmdAutolock = cPrefix + "autolock_enable";
    String cmdAutolockTime = cPrefix + "autolock_time";
    String cmdEcoMode = cPrefix + "eco_mode";
    String cmdEcoPower = cPrefix + "eco_power";
    String cmdPowerShare = cPrefix + "power_sharing";
    String cmdPhaseSwitch = cPrefix + "phase_switch";
    String cmdHalo = cPrefix + "halo";
    String cmdTimezone = cPrefix + "timezone";

    // Auto Lock switch
    publishDiscoverySwitch(*_client, "autolock", "Auto Lock",
        "mdi:lock-clock", cmdAutolock.c_str(), sTopic_,
        "{% if value_json.autolock == 1 %}1{% else %}0{% endif %}",
        "1", "0");

    // Auto Lock timeout (number)
    publishDiscoveryNumber(*_client, "autolock_time", "Auto Lock Timeout",
        "mdi:timer-lock", cmdAutolockTime.c_str(), sTopic_,
        "{{ value_json.autolock_time | default(60) }}", 10, 600, 10, "s");

    // Eco Smart Mode (select). BAPI esm: 0=Off, 1=Full Green, 2=Solar+Grid
    static const char* ecoOptions[] = {"Off", "Full Green (Solar Only)", "Solar + Grid"};
    publishDiscoverySelect(*_client, "eco_mode", "Eco Smart Mode",
        "mdi:solar-power", cmdEcoMode.c_str(), sTopic_,
        "{% set m = value_json.eco_mode | default(0) %}"
        "{% if m == 0 %}Off{% elif m == 1 %}Full Green (Solar Only){% elif m == 2 %}Solar + Grid{% else %}Off{% endif %}",
        ecoOptions, 3);

    // Eco Smart power %
    publishDiscoveryNumber(*_client, "eco_power", "Eco Smart Solar %",
        "mdi:percent", cmdEcoPower.c_str(), sTopic_,
        "{{ value_json.eco_power | default(100) }}", 0, 100, 5, "%");

    // Power Sharing (switch — dynamic power sharing)
    publishDiscoverySwitch(*_client, "power_sharing", "Dynamic Power Sharing",
        "mdi:transit-connection-variant", cmdPowerShare.c_str(), sTopic_,
        "{% if value_json.power_sharing == 1 %}1{% else %}0{% endif %}",
        "1", "0");

    // Phase Switch
    publishDiscoverySwitch(*_client, "phase_switch", "Phase Switch",
        "mdi:numeric-3-circle", cmdPhaseSwitch.c_str(), sTopic_,
        "{% if value_json.phase_switch == 1 %}1{% else %}0{% endif %}",
        "1", "0");

    // Halo LED brightness (select)
    static const char* haloOptions[] = {"Off", "Low", "Medium", "High"};
    publishDiscoverySelect(*_client, "halo", "Halo LED",
        "mdi:led-on", cmdHalo.c_str(), sTopic_,
        "{% set h = value_json.halo | default(2) %}"
        "{% if h == 0 %}Off{% elif h == 1 %}Low{% elif h == 2 %}Medium{% else %}High{% endif %}",
        haloOptions, 4);

    // Timezone (select — common zones)
    static const char* tzOptions[] = {
        "Australia/Sydney", "Australia/Melbourne", "Australia/Brisbane", "Australia/Perth",
        "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Madrid",
        "America/New_York", "America/Chicago", "America/Los_Angeles",
        "Asia/Tokyo", "Asia/Shanghai", "Asia/Singapore",
        "Pacific/Auckland", "UTC"
    };
    publishDiscoverySelect(*_client, "timezone", "Timezone",
        "mdi:earth", cmdTimezone.c_str(), sTopic_,
        "{{ value_json.timezone | default('UTC') }}",
        tzOptions, 16);

    // Gateway IP (makes it easy to find the web UI from HA)
    publishDiscoveryEntity(*_client, "sensor", "gateway_ip", "Gateway IP",
        "mdi:ip-network", gTopic.c_str(), "{{ value_json.ip | default('') }}");

    // Charger device info (diagnostic)
    publishDiscoveryEntity(*_client, "sensor", "dev_name", "Charger Name",
        "mdi:tag", gTopic.c_str(), "{{ value_json.dev_name | default('') }}");
    publishDiscoveryEntity(*_client, "sensor", "dev_mfg", "Charger Manufacturer",
        "mdi:factory", gTopic.c_str(), "{{ value_json.dev_mfg | default('') }}");
    publishDiscoveryEntity(*_client, "sensor", "dev_model", "BLE Radio Model",
        "mdi:chip", gTopic.c_str(), "{{ value_json.dev_model | default('') }}");
    publishDiscoveryEntity(*_client, "sensor", "dev_fw", "BLE Firmware",
        "mdi:cog", gTopic.c_str(), "{{ value_json.dev_fw | default('') }}");

    // rc22 — diagnostic sensors backed by the rc20/rc21 observability
    // surface (max_reentry, tokens, boot_reason, heap watermark, BLE pause
    // state). All from the gateway-info topic so they update every minute
    // alongside the existing dev_* sensors. Useful for HA alarms on the
    // 'this firmware shouldn't be panicking' invariant and for fleet
    // health dashboards.
    publishDiscoveryEntity(*_client, "sensor", "gateway_fw", "Gateway Firmware",
        "mdi:package-variant", gTopic.c_str(), "{{ value_json.fw | default('') }}");
    publishDiscoveryEntity(*_client, "sensor", "boot_reason", "Last Boot Reason",
        "mdi:restart", gTopic.c_str(), "{{ value_json.boot_reason | default('unknown') }}");
    publishDiscoveryEntity(*_client, "sensor", "max_reentry", "Reentry Tripwire",
        "mdi:shield-bug-outline", gTopic.c_str(), "{{ value_json.max_reentry | default(1) }}");
    publishDiscoveryEntity(*_client, "sensor", "tokens", "Rate-Limit Tokens",
        "mdi:gauge", gTopic.c_str(), "{{ value_json.tokens | default(0) }}", nullptr, nullptr, nullptr, "measurement");
    publishDiscoveryEntity(*_client, "sensor", "heap_min_ever", "Heap Min Watermark",
        "mdi:memory", gTopic.c_str(), "{{ value_json.heap_min_ever | default(0) }}", "B", nullptr, nullptr, "measurement");
    publishDiscoveryEntity(*_client, "sensor", "heap_free", "Heap Free",
        "mdi:memory", gTopic.c_str(), "{{ value_json.heap | default(0) }}", "B", nullptr, nullptr, "measurement");
    publishDiscoveryEntity(*_client, "sensor", "gw_uptime", "Gateway Uptime",
        "mdi:clock-outline", gTopic.c_str(), "{{ value_json.uptime | default(0) }}", "s", "duration", nullptr, "measurement");
    publishDiscoveryEntity(*_client, "sensor", "ble_paused", "BLE Paused",
        "mdi:bluetooth-off", gTopic.c_str(),
        "{% if value_json.ble_paused %}Yes ({{ value_json.ble_pause_remaining_s }}s remaining){% else %}No{% endif %}");
    publishDiscoveryEntity(*_client, "sensor", "chg_grounding", "Charger Grounding",
        "mdi:earth", gTopic.c_str(), "{{ value_json.chg_grounding | default('Unknown') }}");
    publishDiscoveryEntity(*_client, "sensor", "wifi_rssi", "WiFi Signal",
        "mdi:wifi", gTopic.c_str(), "{{ value_json.wifi_rssi | default(0) }}", "dBm", "signal_strength", nullptr, "measurement");

    Log.println("[MQTT] HA discovery published (sensors + settings + diagnostics)");
}
