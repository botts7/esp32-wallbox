#pragma once
// Check that a lock / unlock (w_lck) actually took, and fall back when the
// charger ignored it (#47).
//
// Measured on a Pulsar MAX (16 Nov 2026), auto-lock off: w_lck 1 locks within
// 3 s, but w_lck 0 is ignored (r_lck stays 1), with auto-lock on or off. What
// unlocks it is auto-lock going from ON to OFF (s_alo N then s_alo 0), with no
// w_lck at all. Writing s_alo 0 when it is already 0 does nothing.
// On a Pulsar Plus 6.7.43 (tester, #47) w_lck is ignored while auto-lock is off
// and works while it is on; whether auto-lock off then unlocks it is still
// being checked, so only the unlock fallback is applied for now.
//
// So after every w_lck the gateway reads r_lck for a few seconds. If an unlock
// didn't take, it toggles auto-lock through ON and back to the user's own
// setting: off -> 60 s -> off, or N -> off -> N (the off step unlocks; N is
// restored straight after, as the user had it). A lock that didn't take is
// logged. The original Pulsar (lock needs its usid, #68) is left as it is.
//
// Pure helpers; the BLE task does the I/O (WallboxBLE::_sendLockVerified).

#include <Arduino.h>

namespace wb_lock_verify {

constexpr uint32_t kVerifyMs = 4000;     // how long to wait for r_lck to follow
constexpr uint32_t kPollMs = 1000;       // between r_lck reads
constexpr uint32_t kToggleGapMs = 1500;  // between the two s_alo writes
constexpr int kToggleOnSecs = 60;        // auto-lock "on" value used for the toggle

// 1 = lock, 0 = unlock, -1 = not a plain lock parameter. The original
// Pulsar's par carries ',"usid":N' after the digit; only the digit counts.
int wantedOf(const char* par);

// The lock state in an r_lck reply (0 / 1), or -1 when unreadable.
int lockOf(const String& reply);

// The auto-lock timeout (seconds, 0 = off) in a g_alo reply, or -1.
int autolockOf(const String& reply);

// The two s_alo values that take auto-lock through ON and back to the user's
// setting: {60, 0} when it was off, {0, N} when it was N.
struct Toggle { int first; int second; };
Toggle unlockToggle(int originalSecs);

}  // namespace wb_lock_verify
