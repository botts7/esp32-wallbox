// See wb_stop_guard.h.
#include "wb_stop_guard.h"

#include <ArduinoJson.h>
#include <string.h>

namespace wb_stop_guard {

static const float kDeliveringKw = 0.5f;

bool isStopFrame(const char* met, const char* par) {
    if (!met || strcmp(met, "w_cha") != 0) return false;
    if (!par) return true;
    // Accept a bare 1 or a quoted "1" as the start; everything else stops.
    if (strcmp(par, "1") == 0 || strcmp(par, "\"1\"") == 0) return false;
    return true;
}

bool delivering(const String& rdatReply) {
    if (rdatReply.isEmpty()) return false;
    JsonDocument d;
    if (deserializeJson(d, rdatReply) != DeserializationError::Ok) return false;
    JsonVariantConst r = d["r"];
    if (!r.is<JsonObjectConst>()) return false;
    if ((r["st"] | -1) == 1) return true;
    float cp = r["cp"] | 0.0f;
    return cp >= kDeliveringKw;
}

const char* skippedReply() {
    return "{\"r\":null,\"skipped\":\"not-charging\"}";
}

const char* unreadableReply() {
    return "{\"error\":{\"code\":-1,\"message\":\"stop not sent: couldn't read charger state\"}}";
}

}  // namespace wb_stop_guard
