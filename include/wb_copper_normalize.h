#pragma once
#include <Arduino.h>

// Copper SB / Copper Business status normalisation (#20).
//
// The Copper *Business* firmware (e.g. prj10-copper-business-pm3, FW 6.7.38)
// speaks the MAX-family BLE protocol (NINA-B22, service ...d701) so BAPI reads
// succeed — but it returns its r_dat / r_dca JSON under DIFFERENT field names
// than the Pulsar line. Every downstream consumer (dashboard status grid, the
// energy-meter panel, MQTT discovery, the charge-interval log) reads the Pulsar
// names (r.st / r.cp / r.en / r.cur / r.L1..L3 ; meter r.v1 / r.p1), so on a
// Copper SB they all silently read nothing → the Status grid shows "--", the
// Max-Current slider sticks at its default, and the meter shows only Current L1.
//
// normaliseStatus()/normaliseMeter() remap the Copper fields onto the canonical
// Pulsar names IN PLACE, so every surface works unchanged — the same approach
// wb_zentri_normalize uses for the cp-less original Pulsar.
//
// STATUS: SCAFFOLD ONLY. The exact Copper field names are unknown until a raw
// dump is captured from a real unit (issue #20). Both functions are currently
// safe no-ops (return false, mutate nothing) so they compile and run harmlessly
// until the mapping is filled in. See the TODO blocks in the .cpp.
namespace wb_copper {

// True if this raw r_dat looks like a Copper-Business status (so the caller can
// log / branch). Cheap string check; no JSON parse. TODO: fill in the marker.
bool looksLikeCopper(const String& rdatJson);

// Remap a Copper r_dat onto the canonical Pulsar field names in place.
// Returns true if it rewrote anything. No-op (false) for non-Copper statuses.
//   fallbackVoltage / meterJson: same role as the Zentri normaliser, in case
//   charge power must be synthesised from phase currents.
bool normaliseStatus(String& rdatJson, float fallbackVoltage, const String& meterJson);

// Remap a Copper r_dca (energy meter) onto the canonical v1/v2/v3 + p1/p2/p3
// field names in place. Returns true if it rewrote anything.
bool normaliseMeter(String& dcaJson);

}  // namespace wb_copper
