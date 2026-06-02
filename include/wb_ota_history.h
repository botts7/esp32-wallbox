#pragma once

#include <Arduino.h>

// Persistent OTA history — last N attempts (success and failure) kept in
// NVS so /info can show what happened during the last few flashes.
// Useful for catching flapping-OTA patterns or evidence a specific
// release misbehaved.

namespace wb_ota_history {

// 5 was too small — Peter's gateway already rolled past 2.4.2 by the
// time he checked because the rc23/rc24/rc25 chain had filled the
// ring. 20 gives a meaningful upgrade history without bloating NVS
// (each entry serialises to ~100 bytes, so the buffer maxes at ~2 KB).
static const uint8_t MAX_ENTRIES = 20;

struct Entry {
    String   kind;           // "ota" (upload event) or "boot" (version reached healthy state)
    uint32_t uptime_s;       // gateway uptime when the event was recorded
    String   version;        // for "ota": running firmware at upload time
                             // for "boot": firmware that successfully reached healthy
    uint32_t size_bytes;     // OTA only
    bool     success;        // OTA only — FILE_END committed (true) vs aborted (false)
    String   reason;         // OTA only — short failure reason or "ok"
};

// Record an OTA upload attempt. `was_running` is the version that
// performed the upload — this is the OLD firmware, since by the time
// we know the NEW version we've already rebooted into it. Pair with
// recordBoot() to get the "did the new version actually run" half.
void recordOta(uint32_t uptime_s, const String& was_running,
               uint32_t size_bytes, bool success, const String& reason);

// Record that a firmware version reached healthy state on this boot.
// Called from wb_health::markHealthy() once WiFi + BLE/MQTT are up
// and steady. Deduplicated — if the most recent entry is already a
// boot with the same version (e.g. multiple markHealthy calls in
// quick succession), it's a no-op. Pair with recordOta() to see the
// full upgrade chronology in /info.
void recordBoot(uint32_t uptime_s, const String& version);

// Return the stored entries as a JSON array string (newest first).
String toJson();

}  // namespace wb_ota_history
