#pragma once
#include <Arduino.h>

// Zentri (pre-BGX "original" Pulsar, #12) status normalisation.
//
// The original Pulsar's r_dat omits the `cp` (charge power, kW) field that
// the newer Plus/MAX firmware reports — it only carries per-phase currents
// L1/L2/L3 (deci-amps). Every gateway consumer (dashboard, MQTT
// charging_power template, the charge-interval log) reads `value_json.r.cp`,
// so on this hardware they all silently read nothing.
//
// normaliseStatus() synthesises `cp` into the raw r_dat JSON in place, so
// every downstream surface works unchanged. Power is derived from the phase
// currents and the supply voltage:
//
//     cp = (L1 + L2 + L3) / 10  x  V  / 1000        [kW]
//
// V is taken — in order of preference — from a fitted Power Meter accessory's
// measured per-phase voltage (exact), else the user-set nominal mains voltage
// (estimate; default 230 V covers UK/EU/AU/Asia single- and 3-phase, with
// 240 for North America Level 2, 200 for Japan, etc.).
namespace wb_zentri {

// Mutates `rdatJson` in place. Returns true if it injected a synthesised cp
// (i.e. this was a cp-less phase-current status). Leaves Plus/MAX statuses
// — which already carry cp — untouched and returns false.
//   meterJson: the most recent cached r_dca response (for measured v1/v2/v3),
//              or empty to skip the meter tier and use fallbackVoltage.
bool normaliseStatus(String& rdatJson, float fallbackVoltage, const String& meterJson);

}  // namespace wb_zentri
