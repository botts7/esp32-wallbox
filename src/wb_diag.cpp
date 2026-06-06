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
static uint32_t _wifiReconnects = 0;
static uint32_t _bleLongest = 0;
static uint32_t _mqttLongest = 0;
static uint32_t _wifiLongest = 0;
static uint32_t _bleLastReconnect = 0;
static uint32_t _mqttLastReconnect = 0;
static uint32_t _wifiLastReconnect = 0;

// loop_max_ms tripwire grace deadline (absolute millis()). 0 = no
// gate active. extendLoopMaxGate() pushes this forward; the main
// loop's gap tracker calls loopMaxGateActive() to decide whether to
// record the gap.
static uint32_t _loopMaxGateUntilMs = 0;

// Smart tripwire ring (task #74). Head points to the next slot to
// write; entries that match _loopEventsCount < MAX_LOOP_EVENTS are
// populated. After the ring fills, _loopEventsCount stays at the
// max and we just overwrite the oldest slot. Single-word writes
// keep this safe for best-effort cross-task reads.
static LoopEvent _loopEvents[MAX_LOOP_EVENTS] = {};
static volatile uint8_t  _loopEventsHead  = 0;
static volatile uint8_t  _loopEventsCount = 0;

// Pending disconnect start times (one outstanding per kind)
static uint32_t _bleDownStart = 0;   // 0 means no outstanding BLE disconnect
static uint32_t _mqttDownStart = 0;  // 0 means no outstanding MQTT disconnect
static uint32_t _wifiDownStart = 0;  // 0 means no outstanding WiFi disconnect

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

static const char* _kindStr(Kind kind) {
    switch (kind) {
        case Kind::BLE:  return "ble";
        case Kind::MQTT: return "mqtt";
        case Kind::WIFI: return "wifi";
    }
    return "?";
}

static void appendEvent(uint32_t startS, uint32_t durationS, Kind kind) {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();
    JsonObject e = arr.add<JsonObject>();
    e["start"] = startS;
    e["dur"]   = durationS;
    e["kind"]  = _kindStr(kind);
    while ((int)arr.size() > MAX_EVENTS) arr.remove(0);
    store(doc);
}

// ---- Public API ----

void reportDisconnect(Kind kind) {
    uint32_t now = millis() / 1000;
    uint32_t* slot =
        (kind == Kind::BLE)  ? &_bleDownStart  :
        (kind == Kind::MQTT) ? &_mqttDownStart :
                               &_wifiDownStart;
    if (*slot != 0) return;  // already tracking — don't double-count
    *slot = now;
    Log.printf("[Diag] %s disconnect started at uptime %us\n",
               _kindStr(kind), now);
}

void reportReconnect(Kind kind) {
    uint32_t now = millis() / 1000;
    uint32_t* downSlot;
    uint32_t* counter;
    uint32_t* longest;
    uint32_t* lastAt;
    switch (kind) {
        case Kind::BLE:
            downSlot = &_bleDownStart;
            counter  = &_bleReconnects;
            longest  = &_bleLongest;
            lastAt   = &_bleLastReconnect;
            break;
        case Kind::MQTT:
            downSlot = &_mqttDownStart;
            counter  = &_mqttReconnects;
            longest  = &_mqttLongest;
            lastAt   = &_mqttLastReconnect;
            break;
        case Kind::WIFI:
        default:
            downSlot = &_wifiDownStart;
            counter  = &_wifiReconnects;
            longest  = &_wifiLongest;
            lastAt   = &_wifiLastReconnect;
            break;
    }
    if (*downSlot == 0) return;  // never saw the corresponding disconnect
    uint32_t dur = now - *downSlot;
    appendEvent(*downSlot, dur, kind);
    (*counter)++;
    if (dur > *longest) *longest = dur;
    *lastAt = now;
    Log.printf("[Diag] %s reconnected after %us (total %u this boot)\n",
               _kindStr(kind), dur, (unsigned)*counter);
    *downSlot = 0;
    // Whichever side reconnected, give the loop_max_ms tripwire a
    // grace window — sync PubSubClient::connect(), the BLE post-
    // reconnect init burst (5 BAPI reads), and an explicit
    // WiFi.reconnect() are all legitimate blocking events the
    // tripwire was never meant to flag.
    extendLoopMaxGate();
}

void extendLoopMaxGate(uint32_t graceMs) {
    uint32_t deadline = millis() + graceMs;
    // Only push the deadline forward, never backwards — overlapping
    // reconnects shouldn't shorten an existing window.
    if (deadline > _loopMaxGateUntilMs) _loopMaxGateUntilMs = deadline;
}

void recordLoopEvent(uint32_t duration_ms) {
    if (duration_ms < LOOP_EVENT_THRESHOLD_MS) return;
    uint8_t slot = _loopEventsHead;
    _loopEvents[slot].uptime_s    = millis() / 1000;
    _loopEvents[slot].duration_ms = duration_ms;
    _loopEventsHead = (slot + 1) % MAX_LOOP_EVENTS;
    if (_loopEventsCount < MAX_LOOP_EVENTS) _loopEventsCount++;
}

void copyLoopEvents(LoopEvent* out, uint8_t& count) {
    uint8_t n = _loopEventsCount;
    if (n > MAX_LOOP_EVENTS) n = MAX_LOOP_EVENTS;
    count = n;
    if (n == 0) return;
    // Walk backward from the most recently written slot so the
    // caller sees newest-first.
    uint8_t idx = (_loopEventsHead + MAX_LOOP_EVENTS - 1) % MAX_LOOP_EVENTS;
    for (uint8_t i = 0; i < n; i++) {
        out[i] = _loopEvents[idx];
        idx = (idx + MAX_LOOP_EVENTS - 1) % MAX_LOOP_EVENTS;
    }
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
uint32_t wifiReconnects()            { return _wifiReconnects; }
uint32_t bleLongestDurationS()       { return _bleLongest; }
uint32_t mqttLongestDurationS()      { return _mqttLongest; }
uint32_t wifiLongestDurationS()      { return _wifiLongest; }
uint32_t bleLastReconnectUptimeS()   { return _bleLastReconnect; }
uint32_t mqttLastReconnectUptimeS()  { return _mqttLastReconnect; }
uint32_t wifiLastReconnectUptimeS()  { return _wifiLastReconnect; }

String toJson() {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();
    // Build a summary wrapper with counters + reversed events (newest first)
    JsonDocument out;
    out["ble_reconnects"]   = _bleReconnects;
    out["mqtt_reconnects"]  = _mqttReconnects;
    out["wifi_reconnects"]  = _wifiReconnects;
    out["ble_longest_s"]    = _bleLongest;
    out["mqtt_longest_s"]   = _mqttLongest;
    out["wifi_longest_s"]   = _wifiLongest;
    out["ble_last_at_s"]    = _bleLastReconnect;
    out["mqtt_last_at_s"]   = _mqttLastReconnect;
    out["wifi_last_at_s"]   = _wifiLastReconnect;
    out["uptime_s"]         = millis() / 1000;  // for "this boot vs prior" split on /info
    JsonArray events = out["events"].to<JsonArray>();
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
        events.add(arr[i]);
    }
    // Smart tripwire ring — newest first. Live RAM only.
    LoopEvent lev[MAX_LOOP_EVENTS];
    uint8_t   lcount = 0;
    copyLoopEvents(lev, lcount);
    JsonArray loops = out["loop_events"].to<JsonArray>();
    for (uint8_t i = 0; i < lcount; i++) {
        JsonObject e = loops.add<JsonObject>();
        e["start"] = lev[i].uptime_s;
        e["dur_ms"] = lev[i].duration_ms;
    }
    String s;
    serializeJson(out, s);
    return s;
}

void clear() {
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.remove(NVS_KEY); p.end(); }
    _bleReconnects = _mqttReconnects = _wifiReconnects = 0;
    _bleLongest = _mqttLongest = _wifiLongest = 0;
    _bleLastReconnect = _mqttLastReconnect = _wifiLastReconnect = 0;
    _bleDownStart = _mqttDownStart = _wifiDownStart = 0;
    _loopMaxGateUntilMs = 0;
    g_loopMaxMs = 0;
    // Wipe the smart-tripwire ring too — same scope as g_loopMaxMs:
    // the "Clear counters" button is for "I want a clean slate of
    // observability data."
    _loopEventsHead = 0;
    _loopEventsCount = 0;
    for (uint8_t i = 0; i < MAX_LOOP_EVENTS; i++) {
        _loopEvents[i] = {};
    }
    Log.println("[Diag] Counters cleared (incl. loop_max_ms + smart tripwire)");
}

}  // namespace wb_diag
