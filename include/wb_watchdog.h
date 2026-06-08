#pragma once

#include <Arduino.h>
#include <esp_task_wdt.h>

// Tiny RAII-style helper around esp_task_wdt_init().
//
// Backport of the pattern from ESPHome PR #16138 (WatchdogManager). The
// motivating bug: OTA upload extends the Task WDT timeout to 60s so the
// blocking flash-erase inside Update.begin() doesn't trip it, but every
// early-return path out of the upload handler — admission denial, magic-
// byte rejection, Update.begin() failure — used to leave the WDT at 60s
// permanently. That defeats the watchdog for the rest of the device's
// uptime: a wedge later in the day would never trip a reboot.
//
// Two ways to use it:
//
//  1. RAII inside a single function — declare on the stack, gets restored
//     when the scope exits, no matter how:
//
//         {
//             wb_wdt::ScopedExtend long_op(60);
//             do_blocking_thing();
//         }  // <-- WDT restored here
//
//  2. Explicit lifecycle across callbacks (OTA uses this since the upload
//     spans multiple WebServer callback invocations):
//
//         wb_wdt::extendTo(60);   // FILE_START
//         ...
//         wb_wdt::restore();      // FILE_END / ABORTED / any terminal state
//
// Both routes track the original timeout in a module-private static and
// restore it on `restore()` (or `~ScopedExtend`). Idempotent — calling
// restore() twice or without a prior extend is a safe no-op.

namespace wb_wdt {

// Default WDT timeout the project boots with. Arduino-ESP32 doesn't
// expose esp_task_wdt_get_timeout() until 3.x, so we mirror what we set
// at boot. CONFIG_ESP_TASK_WDT_TIMEOUT_S defaults to 5 in the framework;
// adjust here if main.cpp ever calls esp_task_wdt_init() with a different
// value at startup.
static const uint32_t DEFAULT_WDT_TIMEOUT_S = 5;

void extendTo(uint32_t seconds);
void restore();

class ScopedExtend {
public:
    explicit ScopedExtend(uint32_t seconds) { extendTo(seconds); }
    ~ScopedExtend() { restore(); }
    ScopedExtend(const ScopedExtend&) = delete;
    ScopedExtend& operator=(const ScopedExtend&) = delete;
};

}  // namespace wb_wdt
