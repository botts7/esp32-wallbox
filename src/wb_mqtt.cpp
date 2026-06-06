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

// Static option arrays for the HA discovery `select` entities. Lifted
// out of sendDiscovery() so the state-machine switch can reference
// them; declared `const char*` (without inner const) to match the
// publishDiscoverySelect signature.
static const char* kEcoOptions[]  = {"Off", "Full Green (Solar Only)", "Solar + Grid"};
static const char* kHaloOptions[] = {"Off", "Low", "Medium", "High"};
static const char* kTzOptions[]   = {
    "Australia/Sydney", "Australia/Melbourne", "Australia/Brisbane", "Australia/Perth",
    "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Madrid",
    "America/New_York", "America/Chicago", "America/Los_Angeles",
    "Asia/Tokyo", "Asia/Shanghai", "Asia/Singapore",
    "Pacific/Auckland", "UTC"
};

// Total number of discovery entities the state machine publishes.
// Keep in sync with the cases in tickDiscovery(). Bumping this requires
// adding a new case and renumbering nothing — cases are dense 0..N-1.
static const size_t kDiscoveryCount = 57;

// ---- HA Discovery helpers ----

// Single source of truth for the `device` block embedded in every HA
// discovery payload. Previously each helper (entity/switch/number/
// button/select/binary_sensor) built this inline and they drifted —
// only the entity helper carried sw_version, so HA's "merge by
// identifiers" semantics flickered between payloads depending on
// arrival order. peter-mcc 2.4.1 follow-up.
//
// Includes:
//   - identifiers:    the haDeviceId, stable across reboots
//   - name/mfg/model: product info from configMgr
//   - sw_version:     live charger app FW once BLE has read fw_v_;
//                     falls back to the gateway version pre-read so
//                     HA always shows *something* during boot
//   - connections:    [["mac", <wifimac>]] so HA can identify the
//                     device by its WiFi MAC even if the friendly
//                     name changes (helps the device-merge logic
//                     when a user renames in HA UI)
//   - configuration_url: deep link to the gateway dashboard from
//                       the HA Device page header
static void populateDeviceBlock(JsonObject dev) {
    const WBConfig& cfg = configMgr.get();
    dev["identifiers"][0] = cfg.haDeviceId;
    const char* _fullName = nullptr; const char* _shortName = nullptr;
    configMgr.productName(_fullName, _shortName);
    dev["name"]         = _fullName;
    dev["manufacturer"] = "Wallbox";
    dev["model"]        = _shortName;
    // Live charger FW once BLE init has read fw_v_ — otherwise fall
    // back to the gateway version so HA never renders an empty string.
    {
        String fw = wallboxBLE.chargerAppFirmware();
        dev["sw_version"] = fw.length() ? fw : String(WB_VERSION);
    }
    // MAC connection — uses the gateway's WiFi MAC. Helps HA's device
    // registry merge entities that arrive via different code paths.
    {
        String mac = WiFi.macAddress();  // upper-case colon-separated
        mac.toLowerCase();
        JsonArray conns = dev["connections"].to<JsonArray>();
        JsonArray pair  = conns.add<JsonArray>();
        pair.add("mac");
        pair.add(mac);
    }
    // Direct link to the gateway dashboard from the HA Device page
    // header. STA mode -> use the WiFi IP; AP fallback skipped (HA
    // can't reach a captive-portal address anyway).
    if (WiFi.status() == WL_CONNECTED) {
        String url = "http://" + WiFi.localIP().toString() + "/";
        dev["configuration_url"] = url;
    }
}

static void publishDiscoveryEntity(PubSubClient& mqtt, const char* component,
    const char* objectId, const char* name, const char* icon,
    const char* stateTopic, const char* valTemplate,
    const char* unit = nullptr, const char* devClass = nullptr,
    const char* cmdTopic = nullptr, const char* stateClass = nullptr,
    const char* entityCategory = nullptr) {

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
    // HA recognises entity_category="diagnostic" — collapses these
    // entities into a separate "Diagnostic" section on the device
    // page, out of the main Sensors / Controls list. Use for
    // observability counters (max_reentry, loop_max_ms, heap_free,
    // etc.) that are useful for debugging but not for normal use.
    if (entityCategory) doc["entity_category"] = entityCategory;

    // Device block — populated by the shared helper so every helper
    // emits the same fields. See populateDeviceBlock() comment for
    // what's in there. No via_device — the ESP32 gateway IS the device.
    populateDeviceBlock(doc["device"].to<JsonObject>());

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

static void publishDiscoverySwitch(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    const char* payloadOn = "1", const char* payloadOff = "0",
    const char* stateOn = "1", const char* stateOff = "0") {

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
    doc["state_on"] = stateOn;
    doc["state_off"] = stateOff;
    doc["availability_topic"] = availTopic();
    if (icon) doc["icon"] = icon;

    populateDeviceBlock(doc["device"].to<JsonObject>());

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

static void publishDiscoveryNumber(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    float minVal, float maxVal, float step, const char* unit = nullptr,
    const char* mode = nullptr) {

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
    // HA's "box" mode renders an exact-value input field; default is a
    // slider. Used by the Auto Lock Timeout entity so users can type
    // 5/10/30 directly instead of dragging across a 60-step range.
    if (mode) doc["mode"] = mode;

    populateDeviceBlock(doc["device"].to<JsonObject>());

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

    populateDeviceBlock(doc["device"].to<JsonObject>());

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

    populateDeviceBlock(doc["device"].to<JsonObject>());

    String payload;
    serializeJson(doc, payload);
    mqtt.beginPublish(topic.c_str(), payload.length(), true);
    mqtt.print(payload);
    mqtt.endPublish();
}

// ---- MQTT Implementation ----

void WallboxMQTT::begin() {
    const WBConfig& cfg = configMgr.get();
    // Bound per-publish socket blocking. PubSubClient writes are sync
    // and lwIP's TCP write timeout defaults to whatever the kernel
    // says (frequently 5-30 s). On a wedged broker each publish ends
    // up consuming the full timeout, and the ~60-entity discovery
    // burst compounds to a multi-tens-of-seconds main-loop wedge
    // (peter-mcc 2.5.1 saw 80 s). Tightening to 1 s bounds the
    // per-publish worst case at the cost of failing a few publishes
    // that would otherwise have succeeded on a marginal link. The
    // tickDiscovery() state machine retries on the next arm; cached
    // state publishes re-fire on the next BLE seq advance — both
    // self-healing.
    // Arduino-ESP32 WiFiClient::setTimeout takes milliseconds.
    wifiClient.setTimeout(1000);
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
                // 2.7.0 step 8: raw /bapi topic now uses the async
                // queue with MQTT_PUBLISH replyMode. The BLE task
                // drains, executes, and stages the response on the
                // pending-pub ring; the main loop picks it up and
                // publishes to wallbox/response/<met> via the
                // existing publishResponse path. Net: HA sees the
                // same retained payload it always did, but the
                // _mqttCallback no longer blocks main loop for the
                // full BAPI roundtrip.
                wallboxBLE.enqueueRequest(met, par.c_str(),
                    WallboxBLE::ReplyMode::MQTT_PUBLISH);
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
        wallboxBLE.enqueueRequest(bapi::MET_START_STOP, par.c_str());

    } else if (sub == "current") {
        // payload: integer 6-32
        int amps = atoi(payload);
        if (amps < 6 || amps > 32) {
            Log.printf("[CMD] Invalid current: %d (must be 6-32)\n", amps);
            return;
        }
        par = String(amps);
        wallboxBLE.enqueueRequest(bapi::MET_SET_CURRENT, par.c_str());

    } else if (sub == "lock") {
        // payload: "1"/"lock"/"true" or "0"/"unlock"/"false"
        String action = payload;
        action.toLowerCase();
        int val = (action == "1" || action == "lock" || action == "true") ? 1 : 0;
        par = String(val);
        wallboxBLE.enqueueRequest(bapi::MET_LOCK, par.c_str());

    } else if (sub == "reboot") {
        wallboxBLE.enqueueRequest(bapi::MET_REBOOT, "null");

    } else if (sub == "autolock") {
        // payload: JSON {"enabled":1,"time":60}
        wallboxBLE.enqueueRequest(bapi::MET_SET_AUTOLOCK, payload);

    } else if (sub == "eco_smart") {
        wallboxBLE.enqueueRequest(bapi::MET_SET_ECO_SMART, payload);

    } else if (sub == "schedule") {
        wallboxBLE.enqueueRequest(bapi::MET_SET_SCHEDULE, payload);

    } else if (sub == "power_boost") {
        wallboxBLE.enqueueRequest(bapi::MET_SET_POWER_BOOST, payload);

    } else if (sub == "halo") {
        wallboxBLE.enqueueRequest(bapi::MET_SET_HALO, payload);

    // ---- Native HA entity handlers (Batch 1) ----
    } else if (sub == "autolock_enable") {
        // s_alo takes a bare scalar in seconds: 0 = off, N = on, lock
        // after N seconds. Previous version sent an object payload that
        // the charger silently ignored (benvanmierloo PR #9).
        // Toggling ON restores the last-known timeout from BLE polling
        // so the user doesn't see the timeout reset to a default.
        String s = payload; s.toLowerCase();
        bool on = (s == "1" || s == "on" || s == "true");
        int mins = wallboxBLE.lastAutolockMin();
        if (mins < 1) mins = 1;
        int secs = on ? mins * 60 : 0;
        wallboxBLE.enqueueRequest(bapi::MET_SET_AUTOLOCK, String(secs).c_str());

    } else if (sub == "autolock_time") {
        // HA sends minutes (matching the Wallbox app); charger wants
        // seconds. Setting a timeout implies enabling auto-lock.
        int mins = atoi(payload);
        if (mins < 1) mins = 1;
        if (mins > 60) mins = 60;
        wallboxBLE._lastAutolockMin = mins;  // remember for the next switch ON
        wallboxBLE.enqueueRequest(bapi::MET_SET_AUTOLOCK, String(mins * 60).c_str());

    } else if (sub == "eco_mode") {
        // HA sends: "Off", "Full Green (Solar Only)", "Solar + Grid"
        // BAPI: esm=0 disabled, esm=1 Full Green, esm=2 Solar+Grid
        String s = payload;
        int mode = 0;
        if (s.startsWith("Full Green")) mode = 1;
        else if (s == "Solar + Grid") mode = 2;
        String p = "{\"esm\":" + String(mode) + ",\"ese\":" + String(mode > 0 ? 1 : 0) + ",\"esp\":100}";
        wallboxBLE.enqueueRequest(bapi::MET_SET_ECO_SMART, p.c_str());

    } else if (sub == "eco_power") {
        int pct = atoi(payload);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        String p = "{\"esp\":" + String(pct) + "}";
        wallboxBLE.enqueueRequest(bapi::MET_SET_ECO_SMART, p.c_str());

    } else if (sub == "power_sharing") {
        String s = payload; s.toLowerCase();
        int en = (s == "1" || s == "on" || s == "true") ? 1 : 0;
        String p = "{\"dyps\":" + String(en) + "}";
        wallboxBLE.enqueueRequest(bapi::MET_SET_POWER_SHARE, p.c_str());

    } else if (sub == "phase_switch") {
        String s = payload; s.toLowerCase();
        int en = (s == "1" || s == "on" || s == "true") ? 1 : 0;
        String p = "{\"enabled\":" + String(en) + "}";
        wallboxBLE.enqueueRequest(bapi::MET_SET_PHASE, p.c_str());

    } else if (sub == "timezone") {
        // HA sends the timezone string directly
        String p = "{\"timezone\":\"" + String(payload) + "\"}";
        wallboxBLE.enqueueRequest(bapi::MET_SET_TIMEZONE, p.c_str());

    } else {
        Log.printf("[CMD] Unknown command: %s\n", subtopic);
        return;  // unknown — don't bother nudging the poll
    }

    // Any command that reached this point may have changed charger
    // state. Tell the BLE task to re-run its realtime + settings poll
    // on the very next iteration so HA picks up the new state in
    // ~150 ms instead of bouncing through the regular-cadence window.
    wallboxBLE.requestSettingsRepoll();
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

void WallboxMQTT::publishCarConnected(const String& statusJson, const String& realtimeJson) {
    if (!isConnected()) return;
    int st = -1, cs = -1;
    if (!statusJson.isEmpty()) {
        JsonDocument sd;
        if (deserializeJson(sd, statusJson) == DeserializationError::Ok)
            st = sd["r"]["st"] | -1;
    }
    if (!realtimeJson.isEmpty()) {
        JsonDocument rd;
        if (deserializeJson(rd, realtimeJson) == DeserializationError::Ok)
            cs = rd["r"]["charger_status"] | -1;
    }
    // Local r_dat.st codes where a car is physically plugged in.
    bool connected = (st == 1 || st == 2 || st == 3 || st == 4 || st == 5 ||
                      st == 8 || st == 10 || st == 11 || st == 12 || st == 13 || st == 18);
    // Locked (st==6) carries no plug info in the local BLE protocol — Wallbox
    // folds locked/no-car and locked/car-connected into the same code 6.
    // STOPGAP: observed r_sta.charger_status == 19 when locked WITH a car.
    // This is an unverified, firmware-specific heuristic pending a
    // locked-with-NO-car measurement to confirm 19 is plug-specific.
    if (st == 6 && cs == 19) connected = true;
    String topic = baseTopic() + "/car_connected";
    _client->publish(topic.c_str(), connected ? "ON" : "OFF", true);  // retain
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

// =================== HA Discovery state machine ====================
//
// sendDiscovery() ARMS the state machine: populates _discTopics from
// the current config and resets _discoveryIndex to 0. Returns instantly.
// Then on every main-loop iteration (post-MQTT-connected), the main
// task calls tickDiscovery() which publishes EXACTLY ONE entity from
// the switch below and increments the index. When the index reaches
// kDiscoveryCount, the index is set to SIZE_MAX (idle/complete).
//
// Why: PubSubClient is sync. A single ~60-entity burst on a wedged
// broker can compound to tens-of-seconds wall-clock as each
// publish hits the TCP write timeout in series — peter-mcc saw
// loop_max_ms = 80,000 ms overnight with 2.5.1. Per-tick publishing
// bounds the per-iteration cost to ONE publish, so the worst-case
// loop_max_ms during a broker outage is ~1 socket timeout
// (wifiClient.setTimeout(1000) is set in begin()).
//
// On HA's side, autodiscovery is per-message — entities are merged
// by `identifiers` from each payload's device block, so partial bursts
// are safe; HA just gradually populates the device page.
void WallboxMQTT::sendDiscovery() {
    Log.println("[MQTT] HA discovery armed — publishing one entity per main-loop tick");

    // ---- 2.6.0 cleanup: remove discovery for entities that we no
    // longer publish, so existing HA installs don't keep showing them
    // as stale entities. Empty retained payload to the discovery topic
    // is HA's documented "delete this entity" mechanism.
    {
        const WBConfig& cfg = configMgr.get();
        // Status Code (raw) — was case 9 in older code, dropped in
        // 2.6.0 as truly debug-only (the friendly Charger Status
        // sensor is the canonical user-facing value).
        String stale = cfg.haDiscoveryPrefix + "/sensor/" + cfg.haDeviceId + "/charger_status_code/config";
        _client->publish(stale.c_str(), "", true);
    }

    // Populate the topic cache once per arm. The switch cases read
    // from _discTopics by reference.
    _discTopics.sTopic   = statusTopic();
    _discTopics.rTopic   = realtimeTopic();
    _discTopics.gTopic   = baseTopic() + "/response/gateway";
    _discTopics.mTopic   = baseTopic() + "/response/meter";
    _discTopics.nTopic   = baseTopic() + "/response/notifications";
    _discTopics.setTopic = baseTopic() + "/settings";

    String cPrefix = cmdPrefix();
    _discTopics.cmdCurrent       = cPrefix + "current";
    _discTopics.cmdCharging      = cPrefix + "charging";
    _discTopics.cmdLock          = cPrefix + "lock";
    _discTopics.cmdReboot        = cPrefix + "reboot";
    _discTopics.cmdAutolockEnable= cPrefix + "autolock_enable";
    _discTopics.cmdAutolockTime  = cPrefix + "autolock_time";
    _discTopics.cmdEcoMode       = cPrefix + "eco_mode";
    _discTopics.cmdEcoPower      = cPrefix + "eco_power";
    _discTopics.cmdPowerShare    = cPrefix + "power_sharing";
    _discTopics.cmdPhaseSwitch   = cPrefix + "phase_switch";
    _discTopics.cmdHalo          = cPrefix + "halo";
    _discTopics.cmdTimezone      = cPrefix + "timezone";

    _discoveryIndex = 0;
}

void WallboxMQTT::tickDiscovery() {
    if (_discoveryIndex == SIZE_MAX) return;
    if (!isConnected()) return;
    if (_discoveryIndex >= kDiscoveryCount) {
        Log.printf("[MQTT] HA discovery published (%u entities)\n", (unsigned)kDiscoveryCount);
        _discoveryIndex = SIZE_MAX;
        return;
    }

    // Short aliases mirroring the original locals so the switch cases
    // below stay one-for-one with the previous inline body.
    const char* st = _discTopics.sTopic.c_str();
    const char* rt = _discTopics.rTopic.c_str();
    const char* gTopic_c = _discTopics.gTopic.c_str();
    const char* mt = _discTopics.mTopic.c_str();
    const char* nt = _discTopics.nTopic.c_str();
    const char* sTopic_ = _discTopics.setTopic.c_str();

    // Status enum: both MAX and Plus use the same local 0-18 enum
    // per jagheterfredrik/wallbox-ble + benvanmierloo PR #7. The
    // templates below inline that enum directly; car-connected is
    // computed in firmware (publishCarConnected) because st=6 LOCKED
    // carries no plug info on its own and needs r_sta.charger_status
    // to disambiguate.
    switch (_discoveryIndex) {

    // ---- Sensors from r_dat (status topic) ----
    case 0: publishDiscoveryEntity(*_client, "sensor", "charging_power", "Charging Power",
        "mdi:flash", st, "{{ value_json.r.cp | round(2) }}", "kW", "power", nullptr, "measurement"); break;

    // Charging currents are in deciamps (tenths of amps), divide by 10
    case 1: publishDiscoveryEntity(*_client, "sensor", "current_l1", "Charging Current L1",
        "mdi:current-ac", st, "{{ (value_json.r.L1 / 10) | round(1) }}", "A", "current", nullptr, "measurement"); break;
    case 2: publishDiscoveryEntity(*_client, "sensor", "current_l2", "Charging Current L2",
        "mdi:current-ac", st, "{{ (value_json.r.L2 / 10) | round(1) }}", "A", "current", nullptr, "measurement"); break;
    case 3: publishDiscoveryEntity(*_client, "sensor", "current_l3", "Charging Current L3",
        "mdi:current-ac", st, "{{ (value_json.r.L3 / 10) | round(1) }}", "A", "current", nullptr, "measurement"); break;

    // en/gen/grid are in 10-Wh units (centi-kWh), divide by 100 for kWh
    case 4: publishDiscoveryEntity(*_client, "sensor", "energy_session", "Session Energy",
        "mdi:lightning-bolt", st, "{{ (value_json.r.en / 100) | round(2) }}", "kWh", "energy", nullptr, "total_increasing"); break;
    case 5: publishDiscoveryEntity(*_client, "sensor", "grid_energy", "Grid Energy",
        "mdi:transmission-tower", st, "{{ (value_json.r.grid / 100) | round(2) }}", "kWh", "energy", nullptr, "total_increasing"); break;
    case 6: publishDiscoveryEntity(*_client, "sensor", "green_energy", "Green Energy",
        "mdi:leaf", st, "{{ (value_json.r.gen / 100) | round(2) }}", "kWh", "energy", nullptr, "total_increasing"); break;

    // Discharge energy — populates on bidirectional chargers (Quasar 2 V2H).
    // Always 0 on one-way chargers (Pulsar Plus/MAX, Copper, Commander).
    case 7: publishDiscoveryEntity(*_client, "sensor", "discharge_energy", "Discharge Energy (V2H)",
        "mdi:battery-arrow-up", st, "{{ (value_json.r.den / 1000) | round(3) }}", "kWh", "energy", nullptr, "total_increasing"); break;

    case 8: publishDiscoveryEntity(*_client, "sensor", "status", "Charger Status",
        "mdi:ev-station", st,
        "{% set s = value_json.r.st %}"
        "{% set m = {0:'Ready',1:'Charging',2:'Waiting for Car',3:'Waiting for Schedule',"
        "4:'Paused',5:'Charge Complete',6:'Locked',7:'Error',"
        "8:'Waiting for Current Allocation',9:'Power Sharing Not Configured',"
        "10:'Queued (Power Boost)',11:'Discharging',12:'Waiting for MID Auth',"
        "13:'MID Safety Margin Exceeded',14:'OCPP Unavailable',15:'OCPP Finishing',"
        "16:'OCPP Reserved',17:'Updating',18:'Queued (Eco-Smart)'} %}"
        "{{ m.get(s, 'Code ' ~ s) }}"); break;

    // ---- Sensors from r_sta (realtime topic) ----
    // Case 9 — formerly "Status Code (raw)" exposing r_sta.charger_status
    // as a raw int. Dropped in 2.6.0 as debug-only; the friendly
    // "Charger Status" sensor above (from r_dat.st, case 8) is the
    // canonical user-facing value. Kept as a no-op slot so we don't
    // have to renumber 47 subsequent cases. The existing HA entity is
    // deleted via the empty-retained-payload publish in sendDiscovery()
    // above.
    case 9: break;

    case 10: publishDiscoveryEntity(*_client, "sensor", "lock_status", "Lock Status",
        "mdi:lock", rt,
        "{% if value_json.r.lock_status == 0 %}Unlocked{% else %}Locked{% endif %}"); break;

    case 11: publishDiscoveryEntity(*_client, "sensor", "max_available_current", "Max Available Current",
        "mdi:current-ac", rt, "{{ value_json.r.max_available_current }}", "A"); break;

    case 12: publishDiscoveryEntity(*_client, "sensor", "ocpp_status", "OCPP Status",
        "mdi:lan-connect", rt,
        "{% set s = value_json.r.ocpp_status %}"
        "{% if s == 0 %}Not Available{% elif s == 1 %}Not Configured{% elif s == 2 %}Connected{% elif s == 3 %}Charging{% else %}Code {{ s }}{% endif %}"); break;

    // Binary sensor: car connected — computed in firmware (publishCarConnected)
    // because it needs both r_dat.st AND r_sta.charger_status to
    // disambiguate the locked-with-car case (st=6 carries no plug info).
    case 13: {
        const WBConfig& bsCfg = configMgr.get();
        String topic = bsCfg.haDiscoveryPrefix + "/binary_sensor/" + bsCfg.haDeviceId + "/car_connected/config";
        JsonDocument doc;
        doc["name"] = "Car Connected";
        doc["unique_id"] = bsCfg.haDeviceId + "_car_connected";
        doc["object_id"] = bsCfg.haDeviceId + "_car_connected";
        doc["state_topic"] = baseTopic() + "/car_connected";
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["device_class"] = "plug";
        doc["availability_topic"] = availTopic();
        doc["icon"] = "mdi:ev-plug-type2";
        populateDeviceBlock(doc["device"].to<JsonObject>());
        String pl;
        serializeJson(doc, pl);
        _client->beginPublish(topic.c_str(), pl.length(), true);
        _client->print(pl);
        _client->endPublish();
    } break;

    // ---- Controls (number / switch / button) ----
    case 14: publishDiscoveryNumber(*_client, "max_charging_current", "Max Charging Current",
        "mdi:current-ac", _discTopics.cmdCurrent.c_str(), st,
        "{{ value_json.r.cur }}", 6, 32, 1, "A"); break;

    case 15: publishDiscoverySwitch(*_client, "charging", "Charging",
        "mdi:ev-station", _discTopics.cmdCharging.c_str(), st,
        "{% if value_json.r.st == 1 %}1{% else %}0{% endif %}",
        "start", "stop"); break;

    case 16: publishDiscoverySwitch(*_client, "lock", "Charger Lock",
        "mdi:lock", _discTopics.cmdLock.c_str(), rt,
        "{% if value_json.r.lock_status == 1 %}1{% else %}0{% endif %}",
        "lock", "unlock"); break;

    case 17: publishDiscoveryButton(*_client, "reboot", "Reboot Charger",
        "mdi:restart", _discTopics.cmdReboot.c_str()); break;

    // BLE gateway signal — RSSI to the charger (gTopic).
    case 18: publishDiscoveryEntity(*_client, "sensor", "ble_rssi", "BLE Signal",
        "mdi:bluetooth-connect", gTopic_c,
        "{{ value_json.rssi }}", "dBm", "signal_strength", nullptr, "measurement", "diagnostic"); break;

    // ---- Energy meter sensors (from r_dca, on response/meter topic) ----
    case 19: publishDiscoveryEntity(*_client, "sensor", "mains_voltage", "Mains Voltage",
        "mdi:flash-triangle", mt,
        "{{ value_json.r.v1 }}", "V", "voltage", nullptr, "measurement"); break;
    case 20: publishDiscoveryEntity(*_client, "sensor", "grid_power", "House Power",
        "mdi:home-lightning-bolt", mt,
        "{{ value_json.r.p1 }}", "W", "power", nullptr, "measurement"); break;
    case 21: publishDiscoveryEntity(*_client, "sensor", "meter_current", "House Current",
        "mdi:current-ac", mt,
        "{{ (value_json.r.c1 / 10) | round(1) }}", "A", "current", nullptr, "measurement"); break;
    case 22: publishDiscoveryEntity(*_client, "sensor", "meter_total_energy", "Lifetime Energy",
        "mdi:counter", mt,
        "{{ (value_json.r.e / 1000) | round(1) }}", "kWh", "energy", nullptr, "total_increasing"); break;

    // ---- Charger notifications (r_not, published every 60s) ----
    case 23: publishDiscoveryEntity(*_client, "sensor", "notification_count", "Active Notifications",
        "mdi:bell-alert-outline", nt,
        "{{ value_json.count }}"); break;
    case 24: publishDiscoveryEntity(*_client, "sensor", "notification_latest", "Latest Notification",
        "mdi:bell-outline", nt,
        "{{ value_json.latest or 'None' }}"); break;

    // ---- Settings entities (wallbox/settings merged topic) ----
    case 25: publishDiscoverySwitch(*_client, "autolock", "Auto Lock",
        "mdi:lock-clock", _discTopics.cmdAutolockEnable.c_str(), sTopic_,
        "{% if value_json.autolock == 1 %}1{% else %}0{% endif %}",
        "1", "0"); break;

    case 26: publishDiscoveryNumber(*_client, "autolock_time", "Auto Lock Timeout",
        "mdi:timer-lock", _discTopics.cmdAutolockTime.c_str(), sTopic_,
        "{{ value_json.autolock_time | default(1) }}", 1, 60, 1, "min", "box"); break;

    // Eco Smart Mode (select). BAPI esm: 0=Off, 1=Full Green, 2=Solar+Grid
    case 27: publishDiscoverySelect(*_client, "eco_mode", "Eco Smart Mode",
        "mdi:solar-power", _discTopics.cmdEcoMode.c_str(), sTopic_,
        "{% set m = value_json.eco_mode | default(0) %}"
        "{% if m == 0 %}Off{% elif m == 1 %}Full Green (Solar Only){% elif m == 2 %}Solar + Grid{% else %}Off{% endif %}",
        kEcoOptions, 3); break;

    case 28: publishDiscoveryNumber(*_client, "eco_power", "Eco Smart Solar %",
        "mdi:percent", _discTopics.cmdEcoPower.c_str(), sTopic_,
        "{{ value_json.eco_power | default(100) }}", 0, 100, 5, "%"); break;

    case 29: publishDiscoverySwitch(*_client, "power_sharing", "Dynamic Power Sharing",
        "mdi:transit-connection-variant", _discTopics.cmdPowerShare.c_str(), sTopic_,
        "{% if value_json.power_sharing == 1 %}1{% else %}0{% endif %}",
        "1", "0"); break;

    case 30: publishDiscoverySwitch(*_client, "phase_switch", "Phase Switch",
        "mdi:numeric-3-circle", _discTopics.cmdPhaseSwitch.c_str(), sTopic_,
        "{% if value_json.phase_switch == 1 %}1{% else %}0{% endif %}",
        "1", "0"); break;

    case 31: publishDiscoverySelect(*_client, "halo", "Halo LED",
        "mdi:led-on", _discTopics.cmdHalo.c_str(), sTopic_,
        "{% set h = value_json.halo | default(2) %}"
        "{% if h == 0 %}Off{% elif h == 1 %}Low{% elif h == 2 %}Medium{% else %}High{% endif %}",
        kHaloOptions, 4); break;

    case 32: publishDiscoverySelect(*_client, "timezone", "Timezone",
        "mdi:earth", _discTopics.cmdTimezone.c_str(), sTopic_,
        "{{ value_json.timezone | default('UTC') }}",
        kTzOptions, 16); break;

    // ---- Gateway info diagnostic (gTopic) ----
    case 33: publishDiscoveryEntity(*_client, "sensor", "gateway_ip", "Gateway IP",
        "mdi:ip-network", gTopic_c, "{{ value_json.ip | default('') }}"); break;

    case 34: publishDiscoveryEntity(*_client, "sensor", "dev_name", "Charger Name",
        "mdi:tag", gTopic_c, "{{ value_json.dev_name | default('') }}"); break;
    case 35: publishDiscoveryEntity(*_client, "sensor", "dev_mfg", "Charger Manufacturer",
        "mdi:factory", gTopic_c, "{{ value_json.dev_mfg | default('') }}"); break;
    case 36: publishDiscoveryEntity(*_client, "sensor", "dev_model", "BLE Radio Model",
        "mdi:chip", gTopic_c, "{{ value_json.dev_model | default('') }}"); break;
    // BLE Module FW — the radio chip's firmware. Distinct from the
    // charger's application FW below. Renamed in 2.4.1 from "BLE
    // Firmware" which was being mistaken for the charger app FW.
    case 37: publishDiscoveryEntity(*_client, "sensor", "dev_fw", "BLE Module FW",
        "mdi:cog", gTopic_c, "{{ value_json.dev_fw | default('') }}"); break;
    // Charger application firmware — the version Wallbox app shows.
    case 38: publishDiscoveryEntity(*_client, "sensor", "chg_app_fw", "Charger Firmware",
        "mdi:package-variant-closed", gTopic_c,
        "{{ value_json.chg_app_fw | default('') }}"); break;
    case 39: publishDiscoveryEntity(*_client, "sensor", "chg_project", "Charger Project",
        "mdi:tag-outline", gTopic_c,
        "{{ value_json.chg_project | default('') }}"); break;
    // Lifetime session count from r_ses.last. null on chargers that don't
    // expose a usable count (some Plus firmwares) — render as "None" so
    // the entity goes unavailable instead of sticking at 0 (peter-mcc
    // 2.4.1 follow-up).
    case 40: publishDiscoveryEntity(*_client, "sensor", "chg_sessions", "Total Charging Sessions",
        "mdi:counter", gTopic_c,
        "{% if value_json.chg_sessions is none %}None{% else %}{{ value_json.chg_sessions }}{% endif %}",
        nullptr, nullptr, nullptr, "total_increasing"); break;
    case 41: publishDiscoveryEntity(*_client, "sensor", "chg_power_boost", "Power Boost Limit",
        "mdi:home-lightning-bolt-outline", gTopic_c,
        "{{ value_json.chg_power_boost | default(0) }}", "A", "current", nullptr, "measurement"); break;
    // HA's device_class:lock has INVERTED semantics: on = problem
    // (unlocked), off = no problem (locked). r_lck returns 0/1
    // (unlocked/locked). Publish ON when 0.
    case 42: publishDiscoveryEntity(*_client, "binary_sensor", "chg_lock_state", "Lock State",
        "mdi:lock", gTopic_c,
        "{% if value_json.chg_lock_state == 0 %}ON{% else %}OFF{% endif %}", nullptr, "lock"); break;
    // Charger-side WiFi (gnsta). The `signal` field is a 0-100 quality
    // percentage on MAX (no RSSI in dBm). Plain percentage unit.
    case 43: publishDiscoveryEntity(*_client, "sensor", "chg_net_ssid", "Charger WiFi SSID",
        "mdi:wifi", gTopic_c, "{{ value_json.chg_net_ssid | default('') }}"); break;
    case 44: publishDiscoveryEntity(*_client, "sensor", "chg_net_ip", "Charger IP",
        "mdi:ip-network-outline", gTopic_c, "{{ value_json.chg_net_ip | default('') }}"); break;
    case 45: publishDiscoveryEntity(*_client, "sensor", "chg_net_signal", "Charger WiFi Signal",
        "mdi:wifi", gTopic_c,
        "{{ value_json.chg_net_signal | default(0) }}", "%", nullptr, nullptr, "measurement"); break;

    // ---- Observability diagnostics (gTopic) ----
    // All entity_category="diagnostic" so HA collapses them into the
    // device page's diagnostic section instead of mixing with the main
    // sensors. peter-mcc 2.6.0 follow-up: these are useful for power
    // users / forensics but noise for normal operation.
    case 46: publishDiscoveryEntity(*_client, "sensor", "gateway_fw", "Gateway Firmware",
        "mdi:package-variant", gTopic_c, "{{ value_json.fw | default('') }}",
        nullptr, nullptr, nullptr, nullptr, "diagnostic"); break;
    case 47: publishDiscoveryEntity(*_client, "sensor", "boot_reason", "Last Boot Reason",
        "mdi:restart", gTopic_c, "{{ value_json.boot_reason | default('unknown') }}",
        nullptr, nullptr, nullptr, nullptr, "diagnostic"); break;
    case 48: publishDiscoveryEntity(*_client, "sensor", "max_reentry", "Reentry Tripwire",
        "mdi:shield-bug-outline", gTopic_c, "{{ value_json.max_reentry | default(1) }}",
        nullptr, nullptr, nullptr, nullptr, "diagnostic"); break;
    case 49: publishDiscoveryEntity(*_client, "sensor", "tokens", "Rate-Limit Tokens",
        "mdi:gauge", gTopic_c, "{{ value_json.tokens | default(0) }}",
        nullptr, nullptr, nullptr, "measurement", "diagnostic"); break;
    case 50: publishDiscoveryEntity(*_client, "sensor", "loop_max_ms", "Loop Max ms",
        "mdi:timer-alert-outline", gTopic_c,
        "{{ value_json.loop_max_ms | default(0) }}", "ms", "duration", nullptr, "measurement", "diagnostic"); break;
    case 51: publishDiscoveryEntity(*_client, "sensor", "heap_min_ever", "Heap Min Watermark",
        "mdi:memory", gTopic_c, "{{ value_json.heap_min_ever | default(0) }}", "B", nullptr, nullptr, "measurement", "diagnostic"); break;
    case 52: publishDiscoveryEntity(*_client, "sensor", "heap_free", "Heap Free",
        "mdi:memory", gTopic_c, "{{ value_json.heap | default(0) }}", "B", nullptr, nullptr, "measurement", "diagnostic"); break;
    case 53: publishDiscoveryEntity(*_client, "sensor", "gw_uptime", "Gateway Uptime",
        "mdi:clock-outline", gTopic_c, "{{ value_json.uptime | default(0) }}", "s", "duration", nullptr, "measurement", "diagnostic"); break;
    case 54: publishDiscoveryEntity(*_client, "sensor", "ble_paused", "BLE Paused",
        "mdi:bluetooth-off", gTopic_c,
        "{% if value_json.ble_paused %}Yes ({{ value_json.ble_pause_remaining_s }}s remaining){% else %}No{% endif %}",
        nullptr, nullptr, nullptr, nullptr, "diagnostic"); break;
    case 55: publishDiscoveryEntity(*_client, "sensor", "chg_grounding", "Charger Grounding",
        "mdi:earth", gTopic_c, "{{ value_json.chg_grounding | default('Unknown') }}",
        nullptr, nullptr, nullptr, nullptr, "diagnostic"); break;
    case 56: publishDiscoveryEntity(*_client, "sensor", "wifi_rssi", "WiFi Signal",
        "mdi:wifi", gTopic_c, "{{ value_json.wifi_rssi | default(0) }}", "dBm", "signal_strength", nullptr, "measurement", "diagnostic"); break;

    default:
        // Should be unreachable — kDiscoveryCount guards. Defensive
        // bail-out so a bogus index doesn't loop forever.
        _discoveryIndex = SIZE_MAX;
        return;
    }

    _discoveryIndex++;
}
