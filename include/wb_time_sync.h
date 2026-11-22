#pragma once
// Charger clock sync (Wtime) pacing.
//
// The gateway pushes its SNTP time to the charger every hour, and once on each
// reconnect. Some chargers never answer Wtime (a Copper SB on a Zentri radio,
// #89): each try then costs a 4 s timeout on the BLE link. After a few
// unanswered tries in a row the push backs off to once a day and stops forcing
// a push on every reconnect; any reply restores the hourly pace.
//
// Pure (no Arduino types), so it can be unit-tested on the host.

#include <stdint.h>

namespace wb_time_sync {

constexpr uint32_t kHourlyMs = 3600000UL;        // 1 hour
constexpr uint32_t kBackoffMs = 86400000UL;      // 1 day
constexpr uint8_t kMissesBeforeBackoff = 3;

// Interval to the next push, given how many tries in a row went unanswered.
constexpr uint32_t intervalMs(uint8_t misses) {
    return misses >= kMissesBeforeBackoff ? kBackoffMs : kHourlyMs;
}

// Whether a push is due. lastMs == 0 means none this connection: forced while
// the charger answers, paced like any other once it's backed off.
inline bool due(uint32_t nowMs, uint32_t lastMs, uint32_t lastAnyMs, uint8_t misses) {
    if (lastMs == 0) {
        if (misses < kMissesBeforeBackoff) return true;
        return lastAnyMs == 0 || nowMs - lastAnyMs >= intervalMs(misses);
    }
    return nowMs - lastMs >= intervalMs(misses);
}

// Miss counter after a try: reset on any reply, else count up (saturating).
inline uint8_t afterTry(uint8_t misses, bool replied) {
    if (replied) return 0;
    return misses < 255 ? misses + 1 : misses;
}

}  // namespace wb_time_sync
