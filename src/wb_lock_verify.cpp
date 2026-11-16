// See wb_lock_verify.h.
#include "wb_lock_verify.h"

#include <ArduinoJson.h>

namespace wb_lock_verify {

int wantedOf(const char* par) {
    if (!par) return -1;
    while (*par == ' ' || *par == '"') par++;
    if (*par == '1') return 1;
    if (*par == '0') return 0;
    return -1;
}

static int intIn(const String& reply) {
    if (reply.isEmpty()) return -1;
    JsonDocument d;
    if (deserializeJson(d, reply) != DeserializationError::Ok) return -1;
    if (!d["error"].isNull() || !d["r"].is<int>()) return -1;
    return d["r"].as<int>();
}

int lockOf(const String& reply) {
    const int v = intIn(reply);
    return (v == 0 || v == 1) ? v : -1;
}

int autolockOf(const String& reply) {
    const int v = intIn(reply);
    return v >= 0 ? v : -1;
}

Toggle unlockToggle(int originalSecs) {
    if (originalSecs > 0) return {0, originalSecs};
    return {kToggleOnSecs, 0};
}

}  // namespace wb_lock_verify
