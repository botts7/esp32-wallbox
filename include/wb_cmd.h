#pragma once
#include <Arduino.h>

// Shared /api/command action resolver (#179). The sync (wb_web.cpp) and async
// (wb_web_async.cpp) handlers used to carry byte-identical copies of the
// action -> BAPI met/par mapping, the idempotent start/stop skip, the
// control-owner tagging, the model-aware stop parameter, the resume defensive-
// Stop, and the current clamp. They drifted repeatedly (the Pulsar-Plus stop
// par and the 40 A current ceiling were each fixed in one copy but not the
// other across several releases). This module holds the ONE copy; each handler
// only does its own transport-specific param extraction and response sending.
namespace wb_cmd {

struct Plan {
    // ENQUEUE -> caller enqueues {met, par} on the BLE queue.
    // RESPOND -> caller sends `responseJson` with `statusCode` and returns
    //            (idempotent skip = 200, bad request = 400).
    enum Outcome { ENQUEUE, RESPOND };
    Outcome outcome = ENQUEUE;
    String  met;
    String  par;
    String  responseJson;
    int     statusCode = 200;
};

// Resolve a command. `value` is used only by action=="current"; `metParam` /
// `parParam` only by action=="bapi". May perform transport-agnostic side
// effects on the global charger: wb_control::recordCommand(owner) for
// charge-affecting actions, and — for "resume" while actively charging — a
// defensive Stop enqueued ahead of the s_cmode write. enqueueRequest() copies
// met/par, so the returned Plan only needs to outlive the caller's enqueue call.
Plan buildCommand(const String& action, const String& value,
                  const String& owner, const String& metParam,
                  const String& parParam);

}  // namespace wb_cmd
