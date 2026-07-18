#include "wb_copper_normalize.h"
#include <ArduinoJson.h>

// Copper SB / Business status + meter normalisation — SCAFFOLD (#20).
//
// Filling this in needs the raw JSON from a real Copper unit (requested on #20):
//   /api/charger
//   /api/command?action=bapi&met=r_dat&par=null
//   /api/command?action=bapi&met=r_dca&par=null
//
// Once we have them, replace each TODO with the actual field remap. The TARGET
// (canonical Pulsar) names each function must PRODUCE are:
//
//   r_dat : st (status code)      cp  (charge power, kW)   en (session energy)
//           cur (max current, A)  L1/L2/L3 (phase deci-amps)  gen (override)
//   r_dca : v1/v2/v3 (volts)      p1/p2/p3 (watts)
//
// Keep the transform IN PLACE (mutate the passed String) so every downstream
// consumer works unchanged, exactly like wb_zentri_normalize.

namespace wb_copper {

bool looksLikeCopper(const String& rdatJson) {
    // TODO(#20): identify a Copper-Business status cheaply, e.g. a distinctive
    // field only the Copper firmware emits, or the absence of the Pulsar `st`.
    // Return false until we know the marker so this stays a strict no-op.
    (void)rdatJson;
    return false;
}

bool normaliseStatus(String& rdatJson, float fallbackVoltage, const String& meterJson) {
    (void)fallbackVoltage;
    (void)meterJson;
    if (!looksLikeCopper(rdatJson)) return false;

    // TODO(#20): parse rdatJson, read the Copper field names, and write the
    // canonical Pulsar names (st/cp/en/cur/L1..L3) back into r{}. Pattern:
    //
    //   JsonDocument doc;
    //   if (deserializeJson(doc, rdatJson)) return false;
    //   JsonObject r = doc["r"];
    //   r["st"]  = /* copper status field */;
    //   r["cur"] = /* copper max-current field */;
    //   ... (synthesise cp from L1..L3 + voltage if Copper omits it, like Zentri)
    //   String out; serializeJson(doc, out); rdatJson = out;
    //   return true;
    return false;
}

bool normaliseMeter(String& dcaJson) {
    // TODO(#20): Copper reports "only Current L1" — its r_dca uses different
    // keys for voltage/power. Remap onto v1/v2/v3 + p1/p2/p3 in place.
    (void)dcaJson;
    return false;
}

}  // namespace wb_copper
