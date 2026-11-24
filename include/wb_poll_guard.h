#pragma once
// Guards against BLE stream desync poisoning the status/realtime caches (#89).
//
// If a BAPI command times out while the charger is busy (for example turning
// OCPP off) and its reply arrives late, the stream can shift off by one so
// r_dat receives another command's reply (such as a 21-byte {"r":null}) and
// writes receive an earlier r_dat/r_sta frame. Two layers protect against this:
//  1. _sendCommandDirect matches the reply's JSON "id" against the sent id
//     whenever "id" is present, discarding stale late replies.
//  2. _pollStatus and _pollRealtime validate the expected fields before
//     updating the caches, and consecutive invalid r_dat polls trigger a clean
//     BLE reconnect to resync the stream.
//
// Pure C++ (no Arduino types) so it can be unit-tested on the host.

#include <stdint.h>

namespace wb_poll_guard {

constexpr uint8_t kMaxBadStatusPolls = 3;

// True when a reply's JSON id matches the sent request id, or when the reply
// omitted an integer id (gotId < 0). Only an explicit mismatched id is stale.
constexpr bool isReplyIdMatch(int expectedId, int gotId) {
    return gotId < 0 || gotId == expectedId;
}

// r_dat must carry an integer r.st (non-negative status code).
constexpr bool statusFieldValid(int st) {
    return st >= 0;
}

// r_sta must carry an integer charger_status or lock_status before replacing
// the cached realtime payload.
constexpr bool realtimeFieldsValid(bool hasChargerStatus, bool hasLockStatus) {
    return hasChargerStatus || hasLockStatus;
}

// Consecutive invalid r_dat poll counter: resets to 0 on any valid read, else
// increments with saturation.
constexpr uint8_t afterStatusPoll(uint8_t badStreak, bool valid) {
    return valid ? static_cast<uint8_t>(0)
                 : (badStreak < 255 ? static_cast<uint8_t>(badStreak + 1) : badStreak);
}

// Reconnect BLE to resync the stream once invalid r_dat reads reach the cap.
constexpr bool shouldReconnect(uint8_t badStreak) {
    return badStreak >= kMaxBadStatusPolls;
}

}  // namespace wb_poll_guard
