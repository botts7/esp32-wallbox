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
// Comprehensive common IANA zones. The HA select rejects (and log-spams on)
// any value the charger reports that isn't in this list, so it must cover the
// zones users actually run — the curated 16-zone list missed Europe/Amsterdam
// and most others (#14). This is the populated-region set; exotic zones are
// still possible but rare.
static const char* kTzOptions[]   = {
    "UTC",
    // Europe
    "Europe/London", "Europe/Dublin", "Europe/Lisbon", "Europe/Madrid",
    "Europe/Paris", "Europe/Brussels", "Europe/Amsterdam", "Europe/Berlin",
    "Europe/Zurich", "Europe/Rome", "Europe/Vienna", "Europe/Prague",
    "Europe/Warsaw", "Europe/Copenhagen", "Europe/Stockholm", "Europe/Oslo",
    "Europe/Helsinki", "Europe/Athens", "Europe/Bucharest", "Europe/Sofia",
    "Europe/Budapest", "Europe/Belgrade", "Europe/Kyiv", "Europe/Moscow",
    "Europe/Istanbul",
    // Americas
    "America/New_York", "America/Chicago", "America/Denver",
    "America/Los_Angeles", "America/Phoenix", "America/Anchorage",
    "America/Toronto", "America/Vancouver", "America/Halifax",
    "America/Mexico_City", "America/Bogota", "America/Lima",
    "America/Santiago", "America/Sao_Paulo", "America/Argentina/Buenos_Aires",
    // Asia / Middle East
    "Asia/Tokyo", "Asia/Seoul", "Asia/Shanghai", "Asia/Hong_Kong",
    "Asia/Taipei", "Asia/Singapore", "Asia/Kuala_Lumpur", "Asia/Bangkok",
    "Asia/Jakarta", "Asia/Ho_Chi_Minh", "Asia/Manila", "Asia/Kolkata",
    "Asia/Karachi", "Asia/Dubai", "Asia/Tehran", "Asia/Jerusalem",
    "Asia/Riyadh",
    // Africa
    "Africa/Casablanca", "Africa/Algiers", "Africa/Cairo", "Africa/Lagos",
    "Africa/Nairobi", "Africa/Johannesburg",
    // Australia / Pacific
    "Australia/Perth", "Australia/Darwin", "Australia/Brisbane",
    "Australia/Adelaide", "Australia/Sydney", "Australia/Melbourne",
    "Australia/Hobart", "Pacific/Auckland", "Pacific/Fiji",
    "Pacific/Honolulu"
};

// Total number of discovery entities the state machine publishes.
// Keep in sync with the cases in tickDiscovery(). Bumping this requires
// adding a new case and renumbering nothing — cases are dense 0..N-1.
static const size_t kDiscoveryCount = 64;

// ---------------------------------------------------------------------
// 3.0 task #77: table-driven HA discovery.
//
// Shipped in 3.0 after byte-identical verification against the
// legacy 57-case switch: captured all 56 published discovery
// payloads (1 NOOP slot dropped) from both code paths via
// mosquitto_sub and diff'd the JSON line-by-line — every payload
// matched, no HA re-registration event when the flag flipped.
//
// The default is 1 in this release. The legacy switch is removed.
// The `WB_DISCOVERY_TABLE_DRIVEN` macro is kept so an emergency
// rollback can set it to 0 and reinstate a legacy-style code path
// — but the legacy switch itself no longer exists in source, so
// `=0` builds will produce a sendDiscovery() that does nothing
// (discovery silently no-ops). This is documented so a rollback
// candidate knows what they're getting.
//
// The TopicSlot enum lets entries reference the runtime topic strings
// (which live in WallboxMQTT::_discTopics and are populated by
// sendDiscovery()) without storing raw c_str() pointers in a
// constexpr table. The resolver in _tickDiscoveryFromTable() maps
// slot → live String each tick.
// ---------------------------------------------------------------------
#ifndef WB_DISCOVERY_TABLE_DRIVEN
#define WB_DISCOVERY_TABLE_DRIVEN 1
#endif

namespace wb_disc {

enum class EntityKind : uint8_t {
    SENSOR,         // publishDiscoveryEntity, component="sensor"
    BINARY_SENSOR,  // publishDiscoveryEntity, component="binary_sensor"
    NUMBER,         // publishDiscoveryNumber
    SWITCH,         // publishDiscoverySwitch
    SELECT,         // publishDiscoverySelect
    BUTTON,         // publishDiscoveryButton
    CUSTOM,         // inline build (currently only case 13 — car_connected)
    NOOP,           // reserved-slot placeholder (currently only case 9)
};

enum class TopicSlot : uint8_t {
    NONE,
    STATUS,             // r_dat -> _discTopics.sTopic
    REALTIME,           // r_sta -> _discTopics.rTopic
    GATEWAY,            // response/gateway -> _discTopics.gTopic
    METER,              // response/meter -> _discTopics.mTopic
    NOTIFS,             // response/notifications -> _discTopics.nTopic
    SETTINGS,           // wallbox/settings -> _discTopics.setTopic
    LSE,                // response/r_lse -> _discTopics.lseTopic
    CMD_CURRENT,
    CMD_CHARGING,
    CMD_LOCK,
    CMD_REBOOT,
    CMD_AUTOLOCK_EN,
    CMD_AUTOLOCK_TIME,
    CMD_ECO_MODE,
    CMD_ECO_POWER,
    CMD_POWER_SHARE,
    CMD_PHASE_SWITCH,
    CMD_HALO,
    CMD_TIMEZONE,
    CMD_RESUME_SCHEDULE,
};

struct DiscoveryEntry {
    EntityKind   kind;
    const char*  objectId;
    const char*  name;
    const char*  icon;
    TopicSlot    stateTopic;
    const char*  valueTemplate;
    // Common optional sensor fields. nullptr means "skip in payload."
    const char*  unit;
    const char*  deviceClass;
    const char*  stateClass;
    const char*  category;
    // Control fields. Used per-kind:
    //   NUMBER:  commandTopic + numMin/numMax/numStep + (unit) + numMode
    //   SWITCH:  commandTopic + switchOn/switchOff
    //   SELECT:  commandTopic + selectOptions + selectCount
    //   BUTTON:  commandTopic
    TopicSlot    commandTopic;
    int          numMin;
    int          numMax;
    int          numStep;
    const char*  numMode;
    const char*  switchOn;
    const char*  switchOff;
    // publishDiscoverySelect takes a `const char**` (the outer pointer
    // is not const). Match its signature here so we can pass the
    // field through without a cast. The pointed-to array of strings
    // is logically const at runtime; the helper never mutates it.
    const char** selectOptions;
    uint8_t      selectCount;
};

// The table itself. Filled out below with one entry per case slot in
// the legacy tickDiscovery() switch. When kEntryCount == kDiscoveryCount
// AND every entry has been verified against its legacy case body, the
// switch can be retired (3.0 plan step E).
extern const DiscoveryEntry kEntries[];
extern const size_t kEntryCount;

}  // namespace wb_disc

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

// ---- Shared discovery payload buffer ----
//
// Foundation fix (task #105) for the MQTT-discovery boot-time
// fragmentation source. The 5 publishDiscovery* helpers used to do
// `JsonDocument doc; String payload; serializeJson(doc, payload);`
// per entity — 59 alloc/free cycles in ~60 s right when WiFi + BLE
// + web server are also fighting for heap. Audit measured ~120 KB
// of churn over that window.
//
// Replacement: two file-static instances reused across every
// publish. JsonDocument keeps its allocator pool across clear();
// the String keeps its reserved capacity across `= ""`. Permanent
// RAM cost ~2 KB; boot-time alloc churn eliminated.
//
// Single-thread safety: discovery publishes only run from the main
// task's tickDiscovery() drain (one-publish-per-tick rate-limited
// since task #74). No concurrency on these statics.
namespace {
JsonDocument g_discDoc;
String       g_discPayload;
bool         g_discReserved = false;

void _discPrep() {
    if (!g_discReserved) {
        g_discPayload.reserve(2048);  // timezone select payload is ~2 KB (#14)
        g_discReserved = true;
    }
    g_discDoc.clear();
    g_discPayload = "";  // clears content; capacity preserved
}

void _discSerializeAndPublish(PubSubClient& mqtt, const String& topic) {
    serializeJson(g_discDoc, g_discPayload);
    mqtt.beginPublish(topic.c_str(), g_discPayload.length(), true);
    mqtt.print(g_discPayload);
    mqtt.endPublish();
}
}  // anonymous namespace

static void publishDiscoveryEntity(PubSubClient& mqtt, const char* component,
    const char* objectId, const char* name, const char* icon,
    const char* stateTopic, const char* valTemplate,
    const char* unit = nullptr, const char* devClass = nullptr,
    const char* cmdTopic = nullptr, const char* stateClass = nullptr,
    const char* entityCategory = nullptr) {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/" + component + "/" + cfg.haDeviceId + "/" + objectId + "/config";

    _discPrep();
    g_discDoc["name"] = name;
    g_discDoc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    // HA Core 2026.4 removed `object_id` from the MQTT discovery schema —
    // replaced by `default_entity_id`, which must include the platform
    // prefix (e.g. "sensor.wallbox_pulsar_max_chg_power"). Pre-existing
    // entities aren't renamed; `default_entity_id` only sets the default
    // for first-registration.
    g_discDoc["default_entity_id"] = String(component) + "." + cfg.haDeviceId + "_" + objectId;
    g_discDoc["state_topic"] = stateTopic;
    g_discDoc["value_template"] = valTemplate;
    g_discDoc["availability_topic"] = availTopic();
    if (icon) g_discDoc["icon"] = icon;
    if (unit) g_discDoc["unit_of_measurement"] = unit;
    if (devClass) g_discDoc["device_class"] = devClass;
    if (stateClass) g_discDoc["state_class"] = stateClass;
    if (cmdTopic) g_discDoc["command_topic"] = cmdTopic;
    // HA recognises entity_category="diagnostic" — collapses these
    // entities into a separate "Diagnostic" section on the device
    // page, out of the main Sensors / Controls list. Use for
    // observability counters (max_reentry, loop_max_ms, heap_free,
    // etc.) that are useful for debugging but not for normal use.
    if (entityCategory) g_discDoc["entity_category"] = entityCategory;

    // Device block — populated by the shared helper so every helper
    // emits the same fields. See populateDeviceBlock() comment for
    // what's in there. No via_device — the ESP32 gateway IS the device.
    populateDeviceBlock(g_discDoc["device"].to<JsonObject>());

    _discSerializeAndPublish(mqtt, topic);
}

static void publishDiscoverySwitch(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    const char* payloadOn = "1", const char* payloadOff = "0",
    const char* stateOn = "1", const char* stateOff = "0") {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/switch/" + cfg.haDeviceId + "/" + objectId + "/config";

    _discPrep();
    g_discDoc["name"] = name;
    g_discDoc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    // See publishDiscoveryEntity for the object_id -> default_entity_id
    // migration rationale.
    g_discDoc["default_entity_id"] = String("switch.") + cfg.haDeviceId + "_" + objectId;
    g_discDoc["command_topic"] = cmdTopic;
    g_discDoc["state_topic"] = stateTopic;
    g_discDoc["value_template"] = valTemplate;
    g_discDoc["payload_on"] = payloadOn;
    g_discDoc["payload_off"] = payloadOff;
    g_discDoc["state_on"] = stateOn;
    g_discDoc["state_off"] = stateOff;
    g_discDoc["availability_topic"] = availTopic();
    if (icon) g_discDoc["icon"] = icon;

    populateDeviceBlock(g_discDoc["device"].to<JsonObject>());

    _discSerializeAndPublish(mqtt, topic);
}

static void publishDiscoveryNumber(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    float minVal, float maxVal, float step, const char* unit = nullptr,
    const char* mode = nullptr) {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/number/" + cfg.haDeviceId + "/" + objectId + "/config";

    _discPrep();
    g_discDoc["name"] = name;
    g_discDoc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    // See publishDiscoveryEntity for the object_id -> default_entity_id
    // migration rationale.
    g_discDoc["default_entity_id"] = String("number.") + cfg.haDeviceId + "_" + objectId;
    g_discDoc["command_topic"] = cmdTopic;
    g_discDoc["state_topic"] = stateTopic;
    g_discDoc["value_template"] = valTemplate;
    g_discDoc["min"] = minVal;
    g_discDoc["max"] = maxVal;
    g_discDoc["step"] = step;
    g_discDoc["availability_topic"] = availTopic();
    if (icon) g_discDoc["icon"] = icon;
    if (unit) g_discDoc["unit_of_measurement"] = unit;
    // HA's "box" mode renders an exact-value input field; default is a
    // slider. Used by the Auto Lock Timeout entity so users can type
    // 5/10/30 directly instead of dragging across a 60-step range.
    if (mode) g_discDoc["mode"] = mode;

    populateDeviceBlock(g_discDoc["device"].to<JsonObject>());

    _discSerializeAndPublish(mqtt, topic);
}

static void publishDiscoveryButton(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* payload_press = "1") {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/button/" + cfg.haDeviceId + "/" + objectId + "/config";

    _discPrep();
    g_discDoc["name"] = name;
    g_discDoc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    // See publishDiscoveryEntity for the object_id -> default_entity_id
    // migration rationale.
    g_discDoc["default_entity_id"] = String("button.") + cfg.haDeviceId + "_" + objectId;
    g_discDoc["command_topic"] = cmdTopic;
    g_discDoc["payload_press"] = payload_press;
    g_discDoc["availability_topic"] = availTopic();
    if (icon) g_discDoc["icon"] = icon;

    populateDeviceBlock(g_discDoc["device"].to<JsonObject>());

    _discSerializeAndPublish(mqtt, topic);
}

// Select entity (dropdown) — for Eco Smart mode, Halo LED, Timezone
static void publishDiscoverySelect(PubSubClient& mqtt, const char* objectId,
    const char* name, const char* icon, const char* cmdTopic,
    const char* stateTopic, const char* valTemplate,
    const char** options, int optionCount) {

    const WBConfig& cfg = configMgr.get();
    String topic = cfg.haDiscoveryPrefix + "/select/" + cfg.haDeviceId + "/" + objectId + "/config";

    _discPrep();
    g_discDoc["name"] = name;
    g_discDoc["unique_id"] = cfg.haDeviceId + "_" + objectId;
    // See publishDiscoveryEntity for the object_id -> default_entity_id
    // migration rationale.
    g_discDoc["default_entity_id"] = String("select.") + cfg.haDeviceId + "_" + objectId;
    g_discDoc["command_topic"] = cmdTopic;
    g_discDoc["state_topic"] = stateTopic;
    g_discDoc["value_template"] = valTemplate;
    g_discDoc["availability_topic"] = availTopic();
    if (icon) g_discDoc["icon"] = icon;

    JsonArray opts = g_discDoc["options"].to<JsonArray>();
    for (int i = 0; i < optionCount; i++) opts.add(options[i]);

    populateDeviceBlock(g_discDoc["device"].to<JsonObject>());

    _discSerializeAndPublish(mqtt, topic);
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
        // payload: "start", "stop", "pause", or 1/2
        // w_cha par: 1=start, then EITHER 2 (Pulsar MAX hard stop)
        // OR 0 (Pulsar Plus family pause). Plus chargers don't accept
        // par=2 — they ACK the command but the charger keeps charging.
        // jagheterfredrik/wallbox-ble uses par=0 for the Plus protocol
        // family and that's what peter-mcc's Plus needed (issue #4 follow-up).
        // Note: "resume" is NOT routed here — it has a dedicated sub
        // below that sends s_cmode {"mode":0} (clears schedule override).
        String action = payload;
        action.toLowerCase();
        int val = 0;
        if (action == "start" || action == "1") {
            val = 1;
        } else if (action == "stop" || action == "pause" || action == "2") {
            val = configMgr.isPlusFamily() ? 0 : 2;
        } else {
            Log.printf("[CMD] Unknown charging action: %s\n", payload);
            return;
        }
        par = String(val);
        wallboxBLE.enqueueRequest(bapi::MET_START_STOP, par.c_str());

    } else if (sub == "resume_schedule") {
        // Clears the schedule/eco-smart manual-override flag — what
        // the Wallbox app's Resume button does. Defensive prefix:
        // send Stop first because s_cmode mode=0 rejects (subcode 6)
        // when actively charging. Stop is a no-op when not charging.
        const char* stopPar = configMgr.isPlusFamily() ? "0" : "2";
        wallboxBLE.enqueueRequest(bapi::MET_START_STOP, stopPar);
        wallboxBLE.enqueueRequest("s_cmode", "{\"mode\":0}");

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
    _discTopics.lseTopic = baseTopic() + "/response/r_lse";

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
    _discTopics.cmdResumeSched   = cPrefix + "resume_schedule";

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

#if WB_DISCOVERY_TABLE_DRIVEN
    // 3.0 task #77 — dispatch via the static kEntries[] table at the
    // bottom of this TU. Legacy 57-case switch removed; payloads were
    // verified byte-identical (56/56) before the cutover.
    _tickDiscoveryFromTable(_discoveryIndex);
    _discoveryIndex++;
#else
    // Emergency rollback path. The legacy switch was deleted in 3.0;
    // building with =0 silently no-ops discovery (HA will not see any
    // entities). Restore the switch from git history if you actually
    // need this branch — see commit history for src/wb_mqtt.cpp around
    // the 3.0 release.
    _discoveryIndex = SIZE_MAX;
#endif
}


// =====================================================================
// 3.0 task #77 — table-driven dispatcher + entry table.
//
// The legacy 57-case switch was removed in 3.0 after byte-identical
// verification (56/56 HA discovery payloads matched across both
// paths via mosquitto_sub + JSON diff). Each row in kEntries[] below
// corresponds to one HA entity. Slot 9 is intentionally a NOOP — it
// was a raw-status-code sensor dropped in 2.6.0 and the slot is kept
// to avoid renumbering subsequent entries.
//
// To add an entity: append a row, bump kDiscoveryCount, rebuild.
// The static_assert at the bottom of this block guards against the
// table and the count diverging.
// =====================================================================

namespace wb_disc {

const DiscoveryEntry kEntries[] = {
    // ----- Group 1: r_dat / r_sta sensors (cases 0-12) -----

    /*  0 */ { EntityKind::SENSOR, "charging_power", "Charging Power", "mdi:flash",
               TopicSlot::STATUS, "{{ value_json.r.cp | round(2) }}",
               "kW", "power", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  1 */ { EntityKind::SENSOR, "current_l1", "Charging Current L1", "mdi:current-ac",
               TopicSlot::STATUS, "{{ (value_json.r.L1 / 10) | round(1) }}",
               "A", "current", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  2 */ { EntityKind::SENSOR, "current_l2", "Charging Current L2", "mdi:current-ac",
               TopicSlot::STATUS, "{{ (value_json.r.L2 / 10) | round(1) }}",
               "A", "current", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  3 */ { EntityKind::SENSOR, "current_l3", "Charging Current L3", "mdi:current-ac",
               TopicSlot::STATUS, "{{ (value_json.r.L3 / 10) | round(1) }}",
               "A", "current", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  4 */ { EntityKind::SENSOR, "energy_session", "Session Energy", "mdi:lightning-bolt",
               TopicSlot::STATUS, "{{ (value_json.r.en / 100) | round(2) }}",
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  5 */ { EntityKind::SENSOR, "grid_energy", "Grid Energy", "mdi:transmission-tower",
               TopicSlot::STATUS, "{{ (value_json.r.grid / 100) | round(2) }}",
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  6 */ { EntityKind::SENSOR, "green_energy", "Green Energy", "mdi:leaf",
               TopicSlot::STATUS, "{{ (value_json.r.gen / 100) | round(2) }}",
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  7 */ { EntityKind::SENSOR, "discharge_energy", "Discharge Energy (V2H)", "mdi:battery-arrow-up",
               TopicSlot::STATUS, "{{ (value_json.r.den / 1000) | round(3) }}",
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  8 */ { EntityKind::SENSOR, "status", "Charger Status", "mdi:ev-station",
               TopicSlot::STATUS,
               "{% set s = value_json.r.st %}"
               "{% set m = {0:'Ready',1:'Charging',2:'Waiting for Car',3:'Waiting for Schedule',"
               "4:'Paused',5:'Charge Complete',6:'Locked',7:'Error',"
               "8:'Waiting for Current Allocation',9:'Power Sharing Not Configured',"
               "10:'Queued (Power Boost)',11:'Discharging',12:'Waiting for MID Auth',"
               "13:'MID Safety Margin Exceeded',14:'OCPP Unavailable',15:'OCPP Finishing',"
               "16:'OCPP Reserved',17:'Updating',18:'Queued (Eco-Smart)'} %}"
               "{{ m.get(s, 'Code ' ~ s) }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /*  9 */ { EntityKind::NOOP, nullptr, nullptr, nullptr,
               TopicSlot::NONE, nullptr,
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 10 */ { EntityKind::SENSOR, "lock_status", "Lock Status", "mdi:lock",
               TopicSlot::REALTIME,
               "{% if value_json.r.lock_status == 0 %}Unlocked{% else %}Locked{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 11 */ { EntityKind::SENSOR, "max_available_current", "Max Available Current", "mdi:current-ac",
               TopicSlot::REALTIME, "{{ value_json.r.max_available_current }}",
               "A", nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 12 */ { EntityKind::SENSOR, "ocpp_status", "OCPP Status", "mdi:lan-connect",
               TopicSlot::REALTIME,
               "{% set s = value_json.r.ocpp_status %}"
               "{% if s == 0 %}Not Available{% elif s == 1 %}Not Configured{% elif s == 2 %}Connected{% elif s == 3 %}Charging{% else %}Code {{ s }}{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    // ----- Special: case 13 — car_connected (CUSTOM inline JSON) -----

    /* 13 */ { EntityKind::CUSTOM, "car_connected", nullptr, nullptr,
               TopicSlot::NONE, nullptr,
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    // ----- Group 2: main controls (cases 14-17) -----

    /* 14 */ { EntityKind::NUMBER, "max_charging_current", "Max Charging Current", "mdi:current-ac",
               TopicSlot::STATUS, "{{ value_json.r.cur }}",
               "A", nullptr, nullptr, nullptr,
               TopicSlot::CMD_CURRENT, 6,32,1, nullptr,
               nullptr, nullptr, nullptr, 0 },

    /* 15 */ { EntityKind::SWITCH, "charging", "Charging", "mdi:ev-station",
               TopicSlot::STATUS,
               "{% if value_json.r.st == 1 %}1{% else %}0{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_CHARGING, 0,0,0, nullptr,
               "start", "stop", nullptr, 0 },

    /* 16 */ { EntityKind::SWITCH, "lock", "Charger Lock", "mdi:lock",
               TopicSlot::REALTIME,
               "{% if value_json.r.lock_status == 1 %}1{% else %}0{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_LOCK, 0,0,0, nullptr,
               "lock", "unlock", nullptr, 0 },

    /* 17 */ { EntityKind::BUTTON, "reboot", "Reboot Charger", "mdi:restart",
               TopicSlot::NONE, nullptr,
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_REBOOT, 0,0,0, nullptr,
               nullptr, nullptr, nullptr, 0 },

    // ----- Group 3: meter + BLE rssi (cases 18-22) -----

    /* 18 */ { EntityKind::SENSOR, "ble_rssi", "BLE Signal", "mdi:bluetooth-connect",
               TopicSlot::GATEWAY, "{{ value_json.rssi }}",
               "dBm", "signal_strength", "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 19 */ { EntityKind::SENSOR, "mains_voltage", "Mains Voltage", "mdi:flash-triangle",
               TopicSlot::METER, "{{ value_json.r.v1 }}",
               "V", "voltage", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 20 */ { EntityKind::SENSOR, "grid_power", "House Power", "mdi:home-lightning-bolt",
               TopicSlot::METER, "{{ value_json.r.p1 }}",
               "W", "power", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 21 */ { EntityKind::SENSOR, "meter_current", "House Current", "mdi:current-ac",
               TopicSlot::METER, "{{ (value_json.r.c1 / 10) | round(1) }}",
               "A", "current", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 22 */ { EntityKind::SENSOR, "meter_total_energy", "Lifetime Energy", "mdi:counter",
               TopicSlot::METER, "{{ (value_json.r.e / 1000) | round(1) }}",
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    // ----- Group 4: notifications + settings entities (cases 23-32) -----

    /* 23 */ { EntityKind::SENSOR, "notification_count", "Active Notifications", "mdi:bell-alert-outline",
               TopicSlot::NOTIFS, "{{ value_json.count }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 24 */ { EntityKind::SENSOR, "notification_latest", "Latest Notification", "mdi:bell-outline",
               TopicSlot::NOTIFS, "{{ value_json.latest or 'None' }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 25 */ { EntityKind::SWITCH, "autolock", "Auto Lock", "mdi:lock-clock",
               TopicSlot::SETTINGS,
               "{% if value_json.autolock == 1 %}1{% else %}0{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_AUTOLOCK_EN, 0,0,0, nullptr,
               "1", "0", nullptr, 0 },

    /* 26 */ { EntityKind::NUMBER, "autolock_time", "Auto Lock Timeout", "mdi:timer-lock",
               TopicSlot::SETTINGS,
               "{{ value_json.autolock_time | default(1) }}",
               "min", nullptr, nullptr, nullptr,
               TopicSlot::CMD_AUTOLOCK_TIME, 1,60,1, "box",
               nullptr, nullptr, nullptr, 0 },

    /* 27 */ { EntityKind::SELECT, "eco_mode", "Eco Smart Mode", "mdi:solar-power",
               TopicSlot::SETTINGS,
               "{% set m = value_json.eco_mode | default(0) %}"
               "{% if m == 0 %}Off{% elif m == 1 %}Full Green (Solar Only){% elif m == 2 %}Solar + Grid{% else %}Off{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_ECO_MODE, 0,0,0, nullptr,
               nullptr, nullptr, kEcoOptions, 3 },

    /* 28 */ { EntityKind::NUMBER, "eco_power", "Eco Smart Solar %", "mdi:percent",
               TopicSlot::SETTINGS, "{{ value_json.eco_power | default(100) }}",
               "%", nullptr, nullptr, nullptr,
               TopicSlot::CMD_ECO_POWER, 0,100,5, nullptr,
               nullptr, nullptr, nullptr, 0 },

    /* 29 */ { EntityKind::SWITCH, "power_sharing", "Dynamic Power Sharing", "mdi:transit-connection-variant",
               TopicSlot::SETTINGS,
               "{% if value_json.power_sharing == 1 %}1{% else %}0{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_POWER_SHARE, 0,0,0, nullptr,
               "1", "0", nullptr, 0 },

    /* 30 */ { EntityKind::SWITCH, "phase_switch", "Phase Switch", "mdi:numeric-3-circle",
               TopicSlot::SETTINGS,
               "{% if value_json.phase_switch == 1 %}1{% else %}0{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_PHASE_SWITCH, 0,0,0, nullptr,
               "1", "0", nullptr, 0 },

    /* 31 */ { EntityKind::SELECT, "halo", "Halo LED", "mdi:led-on",
               TopicSlot::SETTINGS,
               "{% set h = value_json.halo | default(2) %}"
               "{% if h == 0 %}Off{% elif h == 1 %}Low{% elif h == 2 %}Medium{% else %}High{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_HALO, 0,0,0, nullptr,
               nullptr, nullptr, kHaloOptions, 4 },

    /* 32 */ { EntityKind::SELECT, "timezone", "Timezone", "mdi:earth",
               TopicSlot::SETTINGS, "{{ value_json.timezone | default('UTC') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_TIMEZONE, 0,0,0, nullptr,
               nullptr, nullptr, kTzOptions,
               (int)(sizeof(kTzOptions) / sizeof(kTzOptions[0])) },

    // ----- Group 5: charger details from gTopic (cases 33-45) -----

    /* 33 */ { EntityKind::SENSOR, "gateway_ip", "Gateway IP", "mdi:ip-network",
               TopicSlot::GATEWAY, "{{ value_json.ip | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 34 */ { EntityKind::SENSOR, "dev_name", "Charger Name", "mdi:tag",
               TopicSlot::GATEWAY, "{{ value_json.dev_name | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 35 */ { EntityKind::SENSOR, "dev_mfg", "Charger Manufacturer", "mdi:factory",
               TopicSlot::GATEWAY, "{{ value_json.dev_mfg | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 36 */ { EntityKind::SENSOR, "dev_model", "BLE Radio Model", "mdi:chip",
               TopicSlot::GATEWAY, "{{ value_json.dev_model | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 37 */ { EntityKind::SENSOR, "dev_fw", "BLE Module FW", "mdi:cog",
               TopicSlot::GATEWAY, "{{ value_json.dev_fw | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 38 */ { EntityKind::SENSOR, "chg_app_fw", "Charger Firmware", "mdi:package-variant-closed",
               TopicSlot::GATEWAY, "{{ value_json.chg_app_fw | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 39 */ { EntityKind::SENSOR, "chg_project", "Charger Project", "mdi:tag-outline",
               TopicSlot::GATEWAY, "{{ value_json.chg_project | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 40 */ { EntityKind::SENSOR, "chg_sessions", "Total Charging Sessions", "mdi:counter",
               TopicSlot::GATEWAY,
               "{% if value_json.chg_sessions is none %}None{% else %}{{ value_json.chg_sessions }}{% endif %}",
               nullptr, nullptr, "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 41 */ { EntityKind::SENSOR, "chg_power_boost", "Power Boost Limit", "mdi:home-lightning-bolt-outline",
               TopicSlot::GATEWAY, "{{ value_json.chg_power_boost | default(0) }}",
               "A", "current", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 42 */ { EntityKind::BINARY_SENSOR, "chg_lock_state", "Lock State", "mdi:lock",
               TopicSlot::GATEWAY,
               "{% if value_json.chg_lock_state == 0 %}ON{% else %}OFF{% endif %}",
               nullptr, "lock", nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 43 */ { EntityKind::SENSOR, "chg_net_ssid", "Charger WiFi SSID", "mdi:wifi",
               TopicSlot::GATEWAY, "{{ value_json.chg_net_ssid | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 44 */ { EntityKind::SENSOR, "chg_net_ip", "Charger IP", "mdi:ip-network-outline",
               TopicSlot::GATEWAY, "{{ value_json.chg_net_ip | default('') }}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 45 */ { EntityKind::SENSOR, "chg_net_signal", "Charger WiFi Signal", "mdi:wifi",
               TopicSlot::GATEWAY, "{{ value_json.chg_net_signal | default(0) }}",
               "%", nullptr, "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    // ----- Group 6: diagnostic-category entities (cases 46-56) -----

    /* 46 */ { EntityKind::SENSOR, "gateway_fw", "Gateway Firmware", "mdi:package-variant",
               TopicSlot::GATEWAY, "{{ value_json.fw | default('') }}",
               nullptr, nullptr, nullptr, "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 47 */ { EntityKind::SENSOR, "boot_reason", "Last Boot Reason", "mdi:restart",
               TopicSlot::GATEWAY, "{{ value_json.boot_reason | default('unknown') }}",
               nullptr, nullptr, nullptr, "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 48 */ { EntityKind::SENSOR, "max_reentry", "Reentry Tripwire", "mdi:shield-bug-outline",
               TopicSlot::GATEWAY, "{{ value_json.max_reentry | default(1) }}",
               nullptr, nullptr, nullptr, "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 49 */ { EntityKind::SENSOR, "tokens", "Rate-Limit Tokens", "mdi:gauge",
               TopicSlot::GATEWAY, "{{ value_json.tokens | default(0) }}",
               nullptr, nullptr, "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 50 */ { EntityKind::SENSOR, "loop_max_ms", "Loop Max ms", "mdi:timer-alert-outline",
               TopicSlot::GATEWAY, "{{ value_json.loop_max_ms | default(0) }}",
               "ms", "duration", "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 51 */ { EntityKind::SENSOR, "heap_min_ever", "Heap Min Watermark", "mdi:memory",
               TopicSlot::GATEWAY, "{{ value_json.heap_min_ever | default(0) }}",
               "B", nullptr, "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 52 */ { EntityKind::SENSOR, "heap_free", "Heap Free", "mdi:memory",
               TopicSlot::GATEWAY, "{{ value_json.heap | default(0) }}",
               "B", nullptr, "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 53 */ { EntityKind::SENSOR, "gw_uptime", "Gateway Uptime", "mdi:clock-outline",
               TopicSlot::GATEWAY, "{{ value_json.uptime | default(0) }}",
               "s", "duration", "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 54 */ { EntityKind::SENSOR, "ble_paused", "BLE Paused", "mdi:bluetooth-off",
               TopicSlot::GATEWAY,
               "{% if value_json.ble_paused %}Yes ({{ value_json.ble_pause_remaining_s }}s remaining){% else %}No{% endif %}",
               nullptr, nullptr, nullptr, "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 55 */ { EntityKind::SENSOR, "chg_grounding", "Charger Grounding", "mdi:earth",
               TopicSlot::GATEWAY, "{{ value_json.chg_grounding | default('Unknown') }}",
               nullptr, nullptr, nullptr, "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 56 */ { EntityKind::SENSOR, "wifi_rssi", "WiFi Signal", "mdi:wifi",
               TopicSlot::GATEWAY, "{{ value_json.wifi_rssi | default(0) }}",
               "dBm", "signal_strength", "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    // Surfaces the Wallbox app's "Schedule paused" + "Solar charging
    // paused" labels. The `gen` field in r_dat is the sticky
    // manual-override flag: 0 = schedule armed (will fire normally),
    // non-zero = schedule paused (override active, regardless of
    // whether the charger is currently charging or stopped). Only the
    // Wallbox app's Resume action clears it back to 0; pressing Start
    // or Stop in our gateway does NOT change it. ON = paused.
    /* 57 */ { EntityKind::BINARY_SENSOR, "schedule_paused", "Schedule Paused", "mdi:calendar-clock",
               TopicSlot::STATUS,
               "{% if (value_json.r.gen | default(0)) != 0 %}ON{% else %}OFF{% endif %}",
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    // Button-press of "Resume Schedule" -> publishes any payload to
    // the resume_schedule command topic; the MQTT callback routes
    // that to s_cmode {"mode":0} which clears r_dat.gen back to 0.
    /* 58 */ { EntityKind::BUTTON, "resume_schedule", "Resume Schedule", "mdi:play-circle",
               TopicSlot::NONE, nullptr,
               nullptr, nullptr, nullptr, nullptr,
               TopicSlot::CMD_RESUME_SCHEDULE, 0,0,0, nullptr,
               nullptr, nullptr, nullptr, 0 },

    // ----- Group N: live-session energy (r_lse) entities (cases 59-63) -----
    // Backed by response/r_lse, published with user_id stripped at the BLE
    // layer. Per-session counters reset when a new session starts, so they
    // are MEASUREMENT not total_increasing.

    /* 59 */ { EntityKind::SENSOR, "green_energy_session", "Green Energy (Session)", "mdi:solar-power",
               TopicSlot::LSE, "{{ value_json.r.green_energy | round(2) }}",
               // total_increasing (not measurement): HA rejects measurement for
               // device_class energy; per-session kWh resets each session and
               // accumulates within it, which total_increasing models (#14).
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 60 */ { EntityKind::SENSOR, "grid_energy_session", "Grid Energy (Session)", "mdi:transmission-tower",
               TopicSlot::LSE, "{{ value_json.r.grid_energy | round(2) }}",
               "kWh", "energy", "total_increasing", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 61 */ { EntityKind::SENSOR, "surplus_power", "Solar Surplus Power", "mdi:solar-power-variant",
               TopicSlot::LSE, "{{ value_json.r.active_feature.surplus_power | round(2) }}",
               "kW", "power", "measurement", nullptr,
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 62 */ { EntityKind::SENSOR, "active_feature", "Active Feature", "mdi:cog-outline",
               TopicSlot::LSE, "{{ value_json.r.active_feature.feature }}",
               nullptr, nullptr, "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },

    /* 63 */ { EntityKind::SENSOR, "control_mode", "Control Mode", "mdi:tune-variant",
               TopicSlot::LSE, "{{ value_json.r.control_mode }}",
               nullptr, nullptr, "measurement", "diagnostic",
               TopicSlot::NONE, 0,0,0, nullptr, nullptr, nullptr, nullptr, 0 },
};

const size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// Compile-time invariant: the table must have exactly the same number
// of slots as the legacy switch. If you add/remove a row above without
// updating kDiscoveryCount (or vice versa), this fails at build time.
static_assert(kEntryCount == kDiscoveryCount,
              "wb_disc::kEntries[] size diverged from kDiscoveryCount — "
              "keep the table and the legacy switch in lock-step until "
              "the migration is complete.");

}  // namespace wb_disc

// ---------------------------------------------------------------------
// Topic resolver + table dispatcher.
//
// Only referenced when WB_DISCOVERY_TABLE_DRIVEN is non-zero, but
// always compiled — keeps build correctness honest even with the
// default-off flag, so we catch typos before the flag flips.
// ---------------------------------------------------------------------

static const char* _resolveTopic(wb_disc::TopicSlot slot,
                                 const class WallboxMQTT*) {
    // Lifted into a free function to keep the dispatcher readable; the
    // WallboxMQTT* is reserved for the day the resolver needs anything
    // beyond _discTopics — for now it's unused (the topics live in the
    // instance, but the resolver below uses the friend-pattern via a
    // dedicated accessor).
    (void)slot;
    return nullptr;
}

void WallboxMQTT::_tickDiscoveryFromTable(size_t index) {
    if (index >= wb_disc::kEntryCount) return;
    const auto& e = wb_disc::kEntries[index];

    auto resolve = [&](wb_disc::TopicSlot s) -> const char* {
        switch (s) {
            case wb_disc::TopicSlot::NONE:             return nullptr;
            case wb_disc::TopicSlot::STATUS:           return _discTopics.sTopic.c_str();
            case wb_disc::TopicSlot::REALTIME:         return _discTopics.rTopic.c_str();
            case wb_disc::TopicSlot::GATEWAY:          return _discTopics.gTopic.c_str();
            case wb_disc::TopicSlot::METER:            return _discTopics.mTopic.c_str();
            case wb_disc::TopicSlot::NOTIFS:           return _discTopics.nTopic.c_str();
            case wb_disc::TopicSlot::SETTINGS:         return _discTopics.setTopic.c_str();
            case wb_disc::TopicSlot::LSE:              return _discTopics.lseTopic.c_str();
            case wb_disc::TopicSlot::CMD_CURRENT:      return _discTopics.cmdCurrent.c_str();
            case wb_disc::TopicSlot::CMD_CHARGING:     return _discTopics.cmdCharging.c_str();
            case wb_disc::TopicSlot::CMD_LOCK:         return _discTopics.cmdLock.c_str();
            case wb_disc::TopicSlot::CMD_REBOOT:       return _discTopics.cmdReboot.c_str();
            case wb_disc::TopicSlot::CMD_AUTOLOCK_EN:  return _discTopics.cmdAutolockEnable.c_str();
            case wb_disc::TopicSlot::CMD_AUTOLOCK_TIME:return _discTopics.cmdAutolockTime.c_str();
            case wb_disc::TopicSlot::CMD_ECO_MODE:     return _discTopics.cmdEcoMode.c_str();
            case wb_disc::TopicSlot::CMD_ECO_POWER:    return _discTopics.cmdEcoPower.c_str();
            case wb_disc::TopicSlot::CMD_POWER_SHARE:  return _discTopics.cmdPowerShare.c_str();
            case wb_disc::TopicSlot::CMD_PHASE_SWITCH: return _discTopics.cmdPhaseSwitch.c_str();
            case wb_disc::TopicSlot::CMD_HALO:         return _discTopics.cmdHalo.c_str();
            case wb_disc::TopicSlot::CMD_TIMEZONE:     return _discTopics.cmdTimezone.c_str();
            case wb_disc::TopicSlot::CMD_RESUME_SCHEDULE: return _discTopics.cmdResumeSched.c_str();
        }
        return nullptr;
    };

    const char* st  = resolve(e.stateTopic);
    const char* cmd = resolve(e.commandTopic);

    switch (e.kind) {
        case wb_disc::EntityKind::SENSOR:
            publishDiscoveryEntity(*_client, "sensor", e.objectId, e.name,
                e.icon, st, e.valueTemplate,
                e.unit, e.deviceClass, cmd, e.stateClass, e.category);
            break;
        case wb_disc::EntityKind::BINARY_SENSOR:
            publishDiscoveryEntity(*_client, "binary_sensor", e.objectId, e.name,
                e.icon, st, e.valueTemplate,
                e.unit, e.deviceClass, cmd, e.stateClass, e.category);
            break;
        case wb_disc::EntityKind::NUMBER:
            publishDiscoveryNumber(*_client, e.objectId, e.name, e.icon,
                cmd, st, e.valueTemplate,
                (float)e.numMin, (float)e.numMax, (float)e.numStep,
                e.unit, e.numMode);
            break;
        case wb_disc::EntityKind::SWITCH:
            publishDiscoverySwitch(*_client, e.objectId, e.name, e.icon,
                cmd, st, e.valueTemplate,
                e.switchOn, e.switchOff);
            break;
        case wb_disc::EntityKind::SELECT:
            publishDiscoverySelect(*_client, e.objectId, e.name, e.icon,
                cmd, st, e.valueTemplate,
                e.selectOptions, e.selectCount);
            break;
        case wb_disc::EntityKind::BUTTON:
            publishDiscoveryButton(*_client, e.objectId, e.name, e.icon, cmd);
            break;
        case wb_disc::EntityKind::CUSTOM:
            // Case 13 (car_connected): publishDiscoveryEntity doesn't
            // handle binary_sensor with payload_on/off + custom
            // availability_topic, so the legacy switch built the
            // JsonDocument inline. Mirror that here.
            if (strcmp(e.objectId, "car_connected") == 0) {
                const WBConfig& bsCfg = configMgr.get();
                String topic = bsCfg.haDiscoveryPrefix + "/binary_sensor/"
                             + bsCfg.haDeviceId + "/car_connected/config";
                // Use the shared discovery buffer (task #105) instead
                // of a fresh JsonDocument + String per call.
                _discPrep();
                g_discDoc["name"] = "Car Connected";
                g_discDoc["unique_id"] = bsCfg.haDeviceId + "_car_connected";
                // See publishDiscoveryEntity for the object_id ->
                // default_entity_id migration rationale.
                g_discDoc["default_entity_id"] = String("binary_sensor.") + bsCfg.haDeviceId + "_car_connected";
                g_discDoc["state_topic"] = baseTopic() + "/car_connected";
                g_discDoc["payload_on"] = "ON";
                g_discDoc["payload_off"] = "OFF";
                g_discDoc["device_class"] = "plug";
                g_discDoc["availability_topic"] = availTopic();
                g_discDoc["icon"] = "mdi:ev-plug-type2";
                populateDeviceBlock(g_discDoc["device"].to<JsonObject>());
                _discSerializeAndPublish(*_client, topic);
            }
            break;
        case wb_disc::EntityKind::NOOP:
            // Reserved slot (case 9 — formerly raw status code,
            // dropped in 2.6.0). Do nothing; the empty-retained-payload
            // delete is published once in sendDiscovery().
            break;
    }
}
