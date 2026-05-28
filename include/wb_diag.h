#pragma once

#include <Arduino.h>

// Disconnect-event tracking — counts and timestamps BLE and MQTT
// reconnects so users (and us during diagnosis) can see flap patterns
// without having to watch the live log buffer.
//
// Events are persisted to NVS so reboots don't wipe the history.
// Write churn is bounded — we only write on transition events
// (disconnect start, reconnect complete), not on every loop.

namespace wb_diag {

enum class Kind : uint8_t { BLE = 0, MQTT = 1 };

static const uint8_t MAX_EVENTS = 20;

struct Event {
    uint32_t uptime_start_s;  // gateway uptime when disconnect was detected
    uint32_t duration_s;      // 0 if still ongoing; set when reconnect succeeds
    Kind     kind;
};

// Call when a disconnect is first detected.
// Records the start time; duration stays 0 until reportReconnect().
void reportDisconnect(Kind kind);

// Call when reconnect succeeds — closes the most recent open event of
// the matching kind by computing duration_s = now - start.
void reportReconnect(Kind kind);

// Counters since boot (volatile — not in NVS).
uint32_t bleReconnects();
uint32_t mqttReconnects();

// Longest duration seen since boot for each kind.
uint32_t bleLongestDurationS();
uint32_t mqttLongestDurationS();

// Last uptime_s at which each kind reconnected (0 if never).
uint32_t bleLastReconnectUptimeS();
uint32_t mqttLastReconnectUptimeS();

// Return the persisted ring as a JSON array (newest first).
String toJson();

// Wipe all stored events (called from a manual "Reset diag" button).
void clear();

}  // namespace wb_diag
