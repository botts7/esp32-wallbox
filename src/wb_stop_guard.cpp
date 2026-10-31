// See wb_stop_guard.h.
#include "wb_stop_guard.h"

#include <ArduinoJson.h>
#include <string.h>

namespace wb_stop_guard {

static const float kDeliveringKw = 0.5f;

Kind kindOf(const char* met, const char* par) {
    if (!met || strcmp(met, "w_cha") != 0) return Kind::None;
    if (par && (strcmp(par, "1") == 0 || strcmp(par, "\"1\"") == 0)) return Kind::Start;
    return Kind::Stop;   // 2 on a MAX, 0 on a Plus / original Pulsar
}

bool sessionStarted(const String& rdatReply) {
    if (rdatReply.isEmpty()) return false;
    JsonDocument d;
    if (deserializeJson(d, rdatReply) != DeserializationError::Ok) return false;
    JsonVariantConst r = d["r"];
    if (!r.is<JsonObjectConst>()) return false;
    const int st = r["st"] | -1;
    if (st == 1 || st == 2) return true;
    float cp = r["cp"] | 0.0f;
    return cp >= kDeliveringKw;
}

Verdict decide(Kind kind, bool recentSameKind, const String& fresh, bool legacyPulsar) {
    if (kind == Kind::None) return Verdict::Send;
    if (recentSameKind) return Verdict::Skip;
    if (legacyPulsar) return Verdict::Send;   // its status codes differ; windows only
    if (fresh.isEmpty()) return Verdict::Unreadable;
    const bool started = sessionStarted(fresh);
    if (kind == Kind::Stop) return started ? Verdict::Send : Verdict::Skip;
    return started ? Verdict::Skip : Verdict::Send;   // Start
}

const char* reason(Kind kind, bool recentSameKind, Verdict v) {
    if (v == Verdict::Unreadable) return "couldn't read the charger state";
    if (v == Verdict::Send) return "";
    if (kind == Kind::Stop)
        return recentSameKind ? "a stop was just sent (a second one would toggle it back on)"
                              : "no started session (a stop would toggle it on)";
    return recentSameKind ? "a start was just sent (a second one would pause the handshake)"
                          : "the session is already started (a start would pause it)";
}

const char* skippedReply(Kind kind) {
    return kind == Kind::Start ? "{\"r\":null,\"skipped\":\"already-started\"}"
                               : "{\"r\":null,\"skipped\":\"not-charging\"}";
}

const char* unreadableReply() {
    return "{\"error\":{\"code\":-1,\"message\":\"not sent: couldn't read charger state\"}}";
}

}  // namespace wb_stop_guard
