#pragma once
// Never send a stop to a charger that isn't charging.
//
// On a Pulsar MAX (measured 2026-10-27, fw 6.13.3) w_cha 2 sent to a session
// that is already Paused (st 4) RESUMES charging: the stop acts as a toggle.
// A stale cached "charging" made the gateway's resume path send exactly that
// second stop right after a real one, which restarted an overnight charge at
// 7 kW and ran it 2 h past the target. Some Pulsar Plus firmware treats w_cha
// as a toggle too (#23).
//
// So every w_cha stop (any parameter except the "1" start), from any path
// (HTTP action=stop / resume, MQTT, raw BAPI passthrough), is checked against
// a FRESH r_dat read on the BLE task right before it goes on the wire.

#include <Arduino.h>

namespace wb_stop_guard {

// A stop within this long of the previous one is skipped: the charger takes
// seconds to report the first, so a fresh read still says "charging" and a
// second stop would land on the paused session and toggle it back on
// (measured: two stops 0.3 s apart restarted a MAX). Shorter than the Charge
// Assistant's 18 s stop-retry, which then gets the fresh-state check.
static const uint32_t kRecentStopMs = 15000;

// True for a w_cha frame that stops / pauses (2 on MAX, 0 on Plus / Zentri).
bool isStopFrame(const char* met, const char* par);

// From a fresh r_dat reply: is the charger delivering right now?
// st 1 = charging on every family; cp (kW) covers the ramp-down tail.
// Returns false for an unparseable / error reply.
bool delivering(const String& rdatReply);

// Reply returned to the caller when a stop is skipped.
const char* skippedReply();
// Reply when the fresh state couldn't be read (stop not sent, caller retries).
const char* unreadableReply();

}  // namespace wb_stop_guard
