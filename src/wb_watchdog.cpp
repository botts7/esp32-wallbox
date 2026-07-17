#include "wb_watchdog.h"
#include "wb_log.h"

namespace wb_wdt {

// Original (= "default") timeout the WDT was running at before any
// extendTo() call. We snapshot it on first extend so restore() can put
// it back to exactly that. Initialised lazily because the project's
// boot path can call extendTo() at any time.
static uint32_t _origTimeoutS = DEFAULT_WDT_TIMEOUT_S;
static bool     _extended     = false;

// The panic flag the WDT boots with. esp_task_wdt_init()'s second arg is
// "panic on timeout": true = an elapsed TWDT resets the box, false = it
// only logs. The framework boots with CONFIG_ESP_TASK_WDT_PANIC=1, so
// true is what we must hand back in restore().
#ifdef CONFIG_ESP_TASK_WDT_PANIC
static const bool PANIC_DEFAULT = true;
#else
static const bool PANIC_DEFAULT = false;
#endif

void extendTo(uint32_t seconds) {
    if (!_extended) {
        // Nothing extended yet — capture the *current* default. We don't
        // have a getter on older Arduino-ESP32, so trust the project-wide
        // default declared in the header (matches what main.cpp uses).
        _origTimeoutS = DEFAULT_WDT_TIMEOUT_S;
    }
    // esp_task_wdt_init() with the same args twice is fine — it just
    // reapplies. `panic=false` matches the OTA path: we don't want the
    // device to panic-reboot during a long erase, just to extend the
    // grace period.
    esp_task_wdt_init(seconds, false);
    _extended = true;
    Log.printf("[WDT] Extended to %us\n", (unsigned)seconds);
}

void restore() {
    if (!_extended) return;
    // Restore the timeout AND the panic flag. Passing `false` here (as we
    // used to) left the WDT unable to actually reset the box for the rest
    // of the uptime: extendTo() drops panic deliberately so a long OTA
    // erase can't reboot us mid-flash, but every restore() after that
    // silently kept the watchdog toothless — a later wedge would log and
    // hang instead of recovering, and #168's crash evidence would be lost
    // with it. Symmetry matters: restore() must undo BOTH of extendTo()'s
    // changes, not just the one it's named after.
    esp_task_wdt_init(_origTimeoutS, PANIC_DEFAULT);
    _extended = false;
    Log.printf("[WDT] Restored to %us (panic=%d)\n",
               (unsigned)_origTimeoutS, PANIC_DEFAULT ? 1 : 0);
}

}  // namespace wb_wdt
