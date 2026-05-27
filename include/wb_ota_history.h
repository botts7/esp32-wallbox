#pragma once

#include <Arduino.h>

// Persistent OTA history — last N attempts (success and failure) kept in
// NVS so /info can show what happened during the last few flashes.
// Useful for catching flapping-OTA patterns or evidence a specific
// release misbehaved.

namespace wb_ota_history {

static const uint8_t MAX_ENTRIES = 5;

struct Entry {
    uint32_t uptime_s;       // gateway uptime when the OTA finished
    String   from_version;   // version of the firmware being replaced
    uint32_t size_bytes;     // bytes received
    bool     success;        // FILE_END committed (true) vs aborted (false)
    String   reason;         // short failure reason or "ok"
};

// Append an entry. Oldest is evicted when over MAX_ENTRIES.
void record(uint32_t uptime_s, const String& from_version,
            uint32_t size_bytes, bool success, const String& reason);

// Return the stored entries as a JSON array string (newest first).
String toJson();

}  // namespace wb_ota_history
