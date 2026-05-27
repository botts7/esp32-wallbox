#include "wb_ota_history.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace wb_ota_history {

static const char* NVS_NS = "wbota";
static const char* NVS_KEY = "hist";  // JSON array of entries

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

void record(uint32_t uptime_s, const String& from_version,
            uint32_t size_bytes, bool success, const String& reason) {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();

    // Append the new entry at the END (we'll re-emit newest-first in toJson)
    JsonObject e = arr.add<JsonObject>();
    e["uptime_s"] = uptime_s;
    e["from"]     = from_version;
    e["bytes"]    = size_bytes;
    e["ok"]       = success;
    e["reason"]   = reason;

    // Evict oldest entries (front of array) until size <= MAX_ENTRIES
    while ((int)arr.size() > MAX_ENTRIES) {
        arr.remove(0);
    }
    store(doc);
}

String toJson() {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();

    // Emit newest-first by reversing
    JsonDocument out;
    JsonArray oa = out.to<JsonArray>();
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
        oa.add(arr[i]);
    }
    String s;
    serializeJson(out, s);
    return s;
}

}  // namespace wb_ota_history
