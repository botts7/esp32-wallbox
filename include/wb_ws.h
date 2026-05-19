#pragma once

#include <Arduino.h>

// WebSocket server on port 81.
// Pushes live state to connected browser clients so the UI never needs
// HTTP polling (which blocks on BLE when the link is flaky).
//
// Message format: {"t":"<type>","d":<payload>}
//   type "status"   — r_sta JSON (charging state, power, current, energy)
//   type "meter"    — r_dca JSON (voltage, power, lifetime energy)
//   type "settings" — merged settings poll JSON (autolock, eco, etc.)
//   type "ble"      — {state, rssi, last_activity_s}

namespace wbws {

void begin();
void loop();
void broadcast(const char* type, const String& jsonPayload);
void broadcastBleHealth(const char* state, int rssi, uint32_t lastActivitySec);
size_t clientCount();

}  // namespace wbws
