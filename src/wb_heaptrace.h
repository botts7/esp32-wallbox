#pragma once
#include <Arduino.h>

// TEMP diagnostic (not for release): a 1 Hz ring of heap + BLE-activity samples
// so a periodic stall can be caught in the act. Chasing a Pulsar Plus gateway
// that stalls /api/status for ~6 s every 15 min with heap-min ~4 KB — we need to
// see whether internal heap craters at that instant and what BLE is doing.
namespace wb_heaptrace {

// ~3 min at 1 Hz. Each sample ~16 B -> ~2.9 KB static RAM.
static const int TRACE_LEN = 180;

void begin();
void tick();               // call every loop; self-throttles to 1 Hz
String toJson();           // newest-last samples for /api/diag/heaptrace

} // namespace wb_heaptrace
