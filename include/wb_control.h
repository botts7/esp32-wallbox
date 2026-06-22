#pragma once

#include <Arduino.h>

// Charge-control coordination helpers (advisory). See docs/control-owner.md.
//
// The *owner* setting itself lives in WBConfig (control_owner). This module
// only tracks the LAST controller/user to issue a charge command, so a
// controller can detect a recent manual (or other-controller) override and
// back off. Purely advisory — the gateway never rejects a command.
namespace wb_control {

// Record that a start/stop/current command was issued. `owner` is the
// caller's id (e.g. "integration"); empty -> recorded as "manual".
void recordCommand(const String& owner);

// Id of the last commander ("" if none yet).
String lastCommandBy();

// Milliseconds since the last command (0xFFFFFFFF if none yet).
uint32_t lastCommandAgeMs();

}  // namespace wb_control
