#pragma once
// Guard every w_cha (start / stop) against toggling the session.
//
// Measured on a Pulsar MAX Pro (fw 6.13.3), 27-28 Oct 2026: w_cha acts as a
// TOGGLE on an existing session.
//  - A stop (w_cha 2) sent to a session that is already Paused (st 4) RESUMES
//    it: a stale cached "charging" made the gateway's resume path send exactly
//    that, restarting an overnight charge and running it past the target.
//  - A start (w_cha 1) sent to a session that is already started and waiting
//    for the car (st 2) PAUSES it. The Charge Assistant's start re-assert did
//    that at the moment the car began to draw; after an interrupted handshake
//    the car stopped requesting charge until it was replugged.
//  - s_cmode {"mode":0} (hand back) is rejected in st 2.
// Some Pulsar Plus firmware treats w_cha as a toggle too (#23).
//
// So every w_cha, from any path (HTTP action=start/stop/resume, MQTT, raw BAPI
// passthrough), is checked on the BLE task right before it goes on the wire:
//  - stop:  skipped within kRecentMs of the last stop sent (the charger takes
//           seconds to report it); otherwise sent only if a FRESH r_dat shows a
//           started session (st 1 or 2, or >= 0.5 kW).
//  - start: skipped within kRecentMs of the last start sent (the handshake
//           takes 10-15 s); otherwise skipped if a FRESH r_dat shows the
//           session already started (st 1 or 2). st 3 is allowed: it is also
//           "waiting for schedule", from which a start is valid.
// If the fresh state can't be read, nothing is sent and an error is returned
// (the caller retries); a start or a stop resets the other's window.
//
// The status rules use the Pulsar MAX / Plus status codes. The original
// (Zentri, no-WiFi) Pulsar has its own small enum where st 2 is "car connected,
// idle / full", a normal state to START from (#68 snoop), so for it only the
// time windows apply, never the status rules.

#include <Arduino.h>

namespace wb_stop_guard {

static const uint32_t kRecentMs = 15000;       // a second stop within this is skipped
static const uint32_t kRecentStartMs = 20000;  // a second start within this is skipped
// After a stop is actually sent, wait this long before an s_cmode so the
// session has left st 2 (where s_cmode is rejected).
static const uint32_t kSettleBeforeCmodeMs = 3000;

enum class Kind { None, Start, Stop };
enum class Verdict { Send, Skip, Unreadable };

// Which kind of w_cha frame this is (None for any other method).
Kind kindOf(const char* met, const char* par);

// Is a session started right now, from a fresh r_dat reply?
// st 1 (charging) or 2 (waiting for the car), or delivering >= 0.5 kW.
// Returns false for an unparseable / error reply.
bool sessionStarted(const String& rdatReply);

// The decision for a frame of `kind`, given whether the same kind was sent
// within its window and a fresh r_dat (empty when it couldn't be read).
// legacyPulsar: the original Pulsar, where only the time windows apply.
Verdict decide(Kind kind, bool recentSameKind, const String& fresh, bool legacyPulsar);

// Short human reason for the log.
const char* reason(Kind kind, bool recentSameKind, Verdict v);

const char* skippedReply(Kind kind);
const char* unreadableReply();

}  // namespace wb_stop_guard
