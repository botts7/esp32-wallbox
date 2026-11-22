// Host unit test for wb_time_sync.h (Wtime pacing, #89).
// Build and run:  g++ -std=c++17 -I include test/host/test_time_sync.cpp -o t && ./t
#include <cassert>
#include <cstdio>
#include "wb_time_sync.h"

using namespace wb_time_sync;

int main() {
    const uint32_t H = kHourlyMs, D = kBackoffMs;

    // A charger that answers: forced on connect, then hourly.
    assert(due(1000, 0, 0, 0));
    assert(!due(1000 + H - 1, 1000, 1000, 0));
    assert(due(1000 + H, 1000, 1000, 0));
    assert(afterTry(2, true) == 0);              // any reply resets

    // Misses count up and saturate.
    assert(afterTry(0, false) == 1);
    assert(afterTry(255, false) == 255);

    // Below the threshold it stays hourly and still forces on reconnect.
    assert(intervalMs(kMissesBeforeBackoff - 1) == H);
    assert(due(5000, 0, 1000, kMissesBeforeBackoff - 1));

    // Backed off (Copper SB on a Zentri radio, #89): once a day, and a
    // reconnect no longer forces a try.
    const uint8_t m = kMissesBeforeBackoff;
    assert(intervalMs(m) == D);
    assert(!due(1000 + H, 1000, 1000, m));
    assert(due(1000 + D, 1000, 1000, m));
    assert(!due(1000 + H, 0, 1000, m));          // reconnect within the day
    assert(due(1000 + D, 0, 1000, m));           // reconnect after a day
    assert(due(5000, 0, 0, m));                  // never tried this boot

    // millis() wrap: unsigned subtraction keeps working.
    assert(due(500, 0xFFFFFFFFu - H + 500, 0, 0));

    std::puts("test_time_sync: all passed");
    return 0;
}
