#pragma once
// The charger's own current ceiling (max_available_current: 32, 40 or 48 A),
// kept across reboots.
//
// The gateway only learns it from the first r_dat / r_sta poll after it
// connects, so until then every current path fell back to 32 A: HTTP
// action=current silently sent 32, MQTT cmd/current rejected anything above
// it, and the pages capped their sliders and schedule forms there (a 48 A
// Pulsar Plus user hit this on the forum). The ceiling only changes when the
// charger does, so it's saved when learned and loaded at boot; the first poll
// still overwrites it if the charger reports something else.

#include <Arduino.h>

namespace wb_ceiling {

// The saved ceiling in amps, or -1 when none is saved yet.
int load();

// Save a ceiling learned from the charger (6-80 A; anything else is ignored).
// Writes only when it differs from what's saved.
void save(int amps);

}  // namespace wb_ceiling
