// See wb_bapi_caps.h.
#include "wb_bapi_caps.h"

#include <string.h>

#include "wb_log.h"

namespace wb_bapi_caps {

static const uint8_t MAX_TRACKED = 16;

struct Entry {
    char     met[12];
    uint8_t  errors;       // consecutive error replies
    bool     unsupported;
    uint32_t lastTryMs;    // millis() of the last attempt while unsupported
};

static Entry _entries[MAX_TRACKED];
static uint8_t _count = 0;

static Entry* _find(const char* met, bool create) {
    for (uint8_t i = 0; i < _count; i++)
        if (strncmp(_entries[i].met, met, sizeof(_entries[i].met)) == 0) return &_entries[i];
    if (!create || _count >= MAX_TRACKED) return nullptr;
    Entry& e = _entries[_count++];
    memset(&e, 0, sizeof(e));
    strncpy(e.met, met, sizeof(e.met) - 1);
    return &e;
}

void reset() {
    _count = 0;
}

bool shouldSkip(const char* met) {
    Entry* e = _find(met, false);
    if (!e || !e->unsupported) return false;
    if (millis() - e->lastTryMs >= RETRY_MS) {
        e->lastTryMs = millis();   // let this one through as the retry
        return false;
    }
    return true;
}

void record(const char* met, const String& resp) {
    if (resp.isEmpty()) return;    // timeout: no answer, no information
    JsonDocument d;
    if (deserializeJson(d, resp) != DeserializationError::Ok) return;
    const bool isError = !d["error"].isNull() && d["r"].isNull();
    Entry* e = _find(met, isError);
    if (!e) return;
    if (!isError) {
        if (e->unsupported) Log.printf("[BLE] %s answered again, polling it normally\n", met);
        e->errors = 0;
        e->unsupported = false;
        return;
    }
    if (e->errors < 255) e->errors++;
    if (!e->unsupported && e->errors >= UNSUPPORTED_AFTER) {
        e->unsupported = true;
        e->lastTryMs = millis();
        Log.printf("[BLE] %s unsupported on this charger (%u error replies), "
                   "skipping it, retry every %u min\n",
                   met, (unsigned)e->errors, (unsigned)(RETRY_MS / 60000UL));
    }
}

bool unsupported(const char* met) {
    Entry* e = _find(met, false);
    return e && e->unsupported;
}

void toJson(JsonArray out) {
    for (uint8_t i = 0; i < _count; i++)
        if (_entries[i].unsupported) out.add(_entries[i].met);
}

}  // namespace wb_bapi_caps
