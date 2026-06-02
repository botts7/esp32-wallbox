#include "wb_diag.h"
#include "wb_log.h"
#include <Preferences.h>
#include <ArduinoJson.h>

// loop_max_ms tripwire — defined in wb_web.cpp, written every loop
// iteration by main.cpp. We reset it inside clear() so the existing
// "Clear counters" button on /info also resets the tripwire metric;
// previously it stuck at its boot-time max until the next reboot,
// which left users no recourse for clearing a one-off outlier
// (Peter's 586 ms observation on 2.4.3).
extern volatile uint32_t g_loopMaxMs;

namespace wb_diag {

static const char* NVS_NS = "wbdiag";
static const char* NVS_KEY = "events";

// In-memory state — counters reset on boot; ring buffer mirrored to NVS.
static uint32_t _bleReconnects = 0;
static uint32_t _mqttReconnects = 0;
static uint32_t _bleLongest = 0;
static uint32_t _mqttLongest = 0;
static uint32_t _bleLastReconnect = 0;
static uint32_t _mqttLastReconnect = 0;

// loop_max_ms tripwire grace deadline (absolute millis()). 0 = no
// gate active. extendLoopMaxGate() pushes this forward; the main
// loop's gap tracker calls loopMaxGateActive() to decide whether to
// record the gap.
static uint32_t _loopMaxGateUntilMs = 0;

// Pending disconnect start times (one outstanding per kind)
static uint32_t _bleDownStart = 0;   // 0 means no outstanding BLE disconnect
static uint32_t _mqttDownStart = 0;  // 0 means no outstanding MQTT disconnect

// ---- NVS-backed ring (last MAX_EVENTS, newest at tail) ----

static void load(JsonDocument& doc) {
    Preferences p;
    if (!p.begin(NVS_NS, true)) {
        doc.to<JsonArray>();
        return;
    }
    String s = p.getString(NVS_KEY, "[]");
    p.end();
    if (deserializeJson(doc, s) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        doc.clear();
        doc.to<JsonArray>();
    }
}

static void store(const JsonDocument& doc) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    String s;
    serializeJson(doc, s);
    p.putString(NVS_KEY, s);
    p.end();
}

static void appendEvent(uint32_t startS, uint32_t durationS, Kind kind) {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();
    JsonObject e = arr.add<JsonObject>();
    e["start"] = startS;
    e["dur"]   = durationS;
    e["kind"]  = (kind == Kind::BLE) ? "ble" : "mqtt";
    while ((int)arr.size() > MAX_EVENTS) arr.remove(0);
    store(doc);
}

// ---- Public API ----

void reportDisconnect(Kind kind) {
    uint32_t now = millis() / 1000;
    if (kind == Kind::BLE) {
        if (_bleDownStart != 0) return;  // already tracking — don't double-count
        _bleDownStart = now;
        Log.printf("[Diag] BLE disconnect started at uptime %us\n", now);
    } else {
        if (_mqttDownStart != 0) return;
        _mqttDownStart = now;
        Log.printf("[Diag] MQTT disconnect started at uptime %us\n", now);
    }
}

void reportReconnect(Kind kind) {
    uint32_t now = millis() / 1000;
    if (kind == Kind::BLE) {
        if (_bleDownStart == 0) return;  // we never saw the corresponding disconnect
        uint32_t dur = now - _bleDownStart;
        appendEvent(_bleDownStart, dur, Kind::BLE);
        _bleReconnects++;
        if (dur > _bleLongest) _bleLongest = dur;
        _bleLastReconnect = now;
        Log.printf("[Diag] BLE reconnected after %us (total %u this boot)\n", dur, (unsigned)_bleReconnects);
        _bleDownStart = 0;
    } else {
        if (_mqttDownStart == 0) return;
        uint32_t dur = now - _mqttDownStart;
        appendEvent(_mqttDownStart, dur, Kind::MQTT);
        _mqttReconnects++;
        if (dur > _mqttLongest) _mqttLongest = dur;
        _mqttLastReconnect = now;
        Log.printf("[Diag] MQTT reconnected after %us (total %u this boot)\n", dur, (unsigned)_mqttReconnects);
        _mqttDownStart = 0;
    }
    // Whichever side reconnected, give the loop_max_ms tripwire a
    // grace window — sync PubSubClient::connect() and the BLE
    // post-reconnect init burst (5 BAPI reads) are legitimate
    // blocking events the tripwire was never meant to flag.
    extendLoopMaxGate();
}

void extendLoopMaxGate(uint32_t graceMs) {
    uint32_t deadline = millis() + graceMs;
    // Only push the deadline forward, never backwards — overlapping
    // reconnects shouldn't shorten an existing window.
    if (deadline > _loopMaxGateUntilMs) _loopMaxGateUntilMs = deadline;
}

bool loopMaxGateActive(uint32_t nowMs) {
    if (_loopMaxGateUntilMs == 0) return false;
    if (nowMs >= _loopMaxGateUntilMs) {
        // Gate has expired — clear it so the next reconnect is free
        // to set a fresh deadline and we don't carry a stale value
        // forever.
        _loopMaxGateUntilMs = 0;
        return false;
    }
    return true;
}

uint32_t bleReconnects()             { return _bleReconnects; }
uint32_t mqttReconnects()            { return _mqttReconnects; }
uint32_t bleLongestDurationS()       { return _bleLongest; }
uint32_t mqttLongestDurationS()      { return _mqttLongest; }
uint32_t bleLastReconnectUptimeS()   { return _bleLastReconnect; }
uint32_t mqttLastReconnectUptimeS()  { return _mqttLastReconnect; }

String toJson() {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();
    // Build a summary wrapper with counters + reversed events (newest first)
    JsonDocument out;
    out["ble_reconnects"]   = _bleReconnects;
    out["mqtt_reconnects"]  = _mqttReconnects;
    out["ble_longest_s"]    = _bleLongest;
    out["mqtt_longest_s"]   = _mqttLongest;
    out["ble_last_at_s"]    = _bleLastReconnect;
    out["mqtt_last_at_s"]   = _mqttLastReconnect;
    out["uptime_s"]         = millis() / 1000;  // for "this boot vs prior" split on /info
    JsonArray events = out["events"].to<JsonArray>();
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
        events.add(arr[i]);
    }
    String s;
    serializeJson(out, s);
    return s;
}

void clear() {
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.remove(NVS_KEY); p.end(); }
    _bleReconnects = _mqttReconnects = 0;
    _bleLongest = _mqttLongest = 0;
    _bleLastReconnect = _mqttLastReconnect = 0;
    _bleDownStart = _mqttDownStart = 0;
    _loopMaxGateUntilMs = 0;
    g_loopMaxMs = 0;
    Log.println("[Diag] Counters cleared (incl. loop_max_ms tripwire)");
}

}  // namespace wb_diag
