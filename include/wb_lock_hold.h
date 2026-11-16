#pragma once
// Remembers that the gateway turned auto-lock on to hold a lock (#47).
//
// Some Pulsar Plus firmware (6.7.43) only accepts a lock while auto-lock is on,
// and turning auto-lock off unlocks it (MAX and Plus alike). So when a plain
// lock is ignored, the gateway switches auto-lock on (kHoldSecs), locks, and
// leaves auto-lock on only while the charger stays locked. This records the
// user's own auto-lock setting so it can be put back on unlock, including an
// unlock from the app or RFID (seen in the lock state), and across a reboot.
// Saved in NVS (namespace "wblock").

namespace wb_lock_hold {

constexpr int kHoldSecs = 300;  // auto-lock while held: long enough for the 30 s poll to catch an unlock elsewhere

// The user's auto-lock setting to restore, or -1 when no hold is active.
int heldOriginal();

void markHeld(int originalSecs);
void clear();

}  // namespace wb_lock_hold
