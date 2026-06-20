#include "wb_zentri_normalize.h"
#include <ArduinoJson.h>
#include <math.h>

namespace wb_zentri {

// Resolve the voltage to use for phase `idx` (0=v1,1=v2,2=v3): the meter's
// measured per-phase value if present and sane, else v1 for all phases, else
// the caller's fallback. A fitted Power Meter (r_dca) makes the derivation
// exact and region-agnostic; without one we fall back to the nominal setting.
static float phaseVoltage(JsonObjectConst meter, int idx, float fallback) {
    if (!meter.isNull()) {
        const char* keys[3] = {"v1", "v2", "v3"};
        // Exact phase first.
        if (meter[keys[idx]].is<float>()) {
            float v = meter[keys[idx]].as<float>();
            if (v > 50.0f && v < 500.0f) return v;
        }
        // Single-phase meters only report v1 — reuse it for any phase asked.
        if (meter["v1"].is<float>()) {
            float v = meter["v1"].as<float>();
            if (v > 50.0f && v < 500.0f) return v;
        }
    }
    return fallback;
}

bool normaliseStatus(String& rdatJson, float fallbackVoltage, const String& meterJson) {
    JsonDocument doc;
    if (deserializeJson(doc, rdatJson) != DeserializationError::Ok) return false;
    JsonObject r = doc["r"];
    if (r.isNull()) return false;
    if (!r["cp"].isNull()) return false;   // Plus/MAX already reports power — leave it
    if (r["L1"].isNull()) return false;    // not a phase-current status — nothing to derive

    const int  st = r["st"] | -1;
    const long L1 = r["L1"] | 0;
    const long L2 = r["L2"] | 0;
    const long L3 = r["L3"] | 0;

    // Only count power while the charger reports the active-charging state
    // (st==1 on the Zentri enum: 0=ready,1=charging,2=connected,3=waiting,
    // 4=ramp). Standby/decay current during waiting (st 3/4) would otherwise
    // log phantom charge bursts in the interval capture.
    float cp = 0.0f;
    if (st == 1) {
        // Parse the cached meter once for measured voltages (no meter -> null).
        JsonDocument md;
        JsonObjectConst meter;
        if (!meterJson.isEmpty() &&
            deserializeJson(md, meterJson) == DeserializationError::Ok) {
            meter = md["r"];
        }
        const float p1 = (L1 / 10.0f) * phaseVoltage(meter, 0, fallbackVoltage);
        const float p2 = (L2 / 10.0f) * phaseVoltage(meter, 1, fallbackVoltage);
        const float p3 = (L3 / 10.0f) * phaseVoltage(meter, 2, fallbackVoltage);
        cp = (p1 + p2 + p3) / 1000.0f;     // W -> kW
        if (cp < 0.0f) cp = 0.0f;
    }

    r["cp"] = roundf(cp * 100.0f) / 100.0f;  // 2 dp, mirrors the Plus/MAX cp shape
    rdatJson = "";
    serializeJson(doc, rdatJson);
    return true;
}

}  // namespace wb_zentri
