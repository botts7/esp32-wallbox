// Host unit test for wb_poll_guard.h (BLE desync + cache guard, #89).
// Build and run:  g++ -std=c++17 -I include test/host/test_poll_guard.cpp -o t && ./t
#include <cassert>
#include <cstdio>
#include "wb_poll_guard.h"

using namespace wb_poll_guard;

int main() {
    // Reply id matching: exact match passes; omitted id (-1) passes; mismatch fails.
    assert(isReplyIdMatch(42, 42));
    assert(isReplyIdMatch(42, -1));
    assert(!isReplyIdMatch(42, 41));
    assert(!isReplyIdMatch(42, 43));
    assert(!isReplyIdMatch(1, 0));

    // Status field validation: integer r.st >= 0 required.
    assert(!statusFieldValid(-1));
    assert(statusFieldValid(0));
    assert(statusFieldValid(1));
    assert(statusFieldValid(19));

    // Realtime validation: needs charger_status or lock_status.
    assert(!realtimeFieldsValid(false, false));
    assert(realtimeFieldsValid(true, false));
    assert(realtimeFieldsValid(false, true));
    assert(realtimeFieldsValid(true, true));

    // Consecutive invalid r_dat polls trigger reconnect at kMaxBadStatusPolls.
    uint8_t streak = 0;
    for (uint8_t i = 1; i < kMaxBadStatusPolls; i++) {
        streak = afterStatusPoll(streak, false);
        assert(streak == i);
        assert(!shouldReconnect(streak));
    }
    streak = afterStatusPoll(streak, false);
    assert(streak == kMaxBadStatusPolls);
    assert(shouldReconnect(streak));

    // Any valid status read resets the streak.
    streak = afterStatusPoll(streak, true);
    assert(streak == 0);
    assert(!shouldReconnect(streak));

    // Saturation at 255.
    assert(afterStatusPoll(255, false) == 255);
    assert(afterStatusPoll(255, true) == 0);

    std::puts("test_poll_guard: all passed");
    return 0;
}
