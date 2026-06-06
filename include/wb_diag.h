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

enum class Kind : uint8_t { BLE = 0, MQTT = 1, WIFI = 2 };

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
uint32_t wifiReconnects();

// Longest duration seen since boot for each kind.
uint32_t bleLongestDurationS();
uint32_t mqttLongestDurationS();
uint32_t wifiLongestDurationS();

// Last uptime_s at which each kind reconnected (0 if never).
uint32_t bleLastReconnectUptimeS();
uint32_t mqttLastReconnectUptimeS();
uint32_t wifiLastReconnectUptimeS();

// Return the persisted ring as a JSON array (newest first).
String toJson();

// Wipe all stored events (called from a manual "Reset diag" button).
void clear();

// "Loop-max gate" — peter-mcc 2.4.2 follow-up: the loop_max_ms
// tripwire was meant to catch unprovoked runtime wedges (the
// peter-mcc #4 hours-of-uptime freeze class), but it was also
// triggering on legitimate MQTT/BLE reconnect blocking. Sync
// PubSubClient::connect() can block the main task for ~15 s per
// attempt — a transient broker hiccup easily produces a 30-40 s
// gap that saturates the metric. After every reportReconnect()
// call we now extend a grace window forward by `graceMs` ms; the
// main loop's tripwire checks loopMaxGateActive(now) and skips
// recording gaps that fall inside it. The metric then surfaces
// only *unprovoked* wedges.
//
// Default grace window is 30 s, which generously covers the worst-
// case sync-connect timeout plus any post-connect work (discovery
// republish, BLE keepalive catch-up).
static const uint32_t LOOP_MAX_GATE_DEFAULT_MS = 30000;

void extendLoopMaxGate(uint32_t graceMs = LOOP_MAX_GATE_DEFAULT_MS);
bool loopMaxGateActive(uint32_t nowMs);

}  // namespace wb_diag
