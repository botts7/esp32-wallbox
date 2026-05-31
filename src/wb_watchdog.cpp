#include "wb_watchdog.h"
#include "wb_log.h"

namespace wb_wdt {

// Original (= "default") timeout the WDT was running at before any
// extendTo() call. We snapshot it on first extend so restore() can put
// it back to exactly that. Initialised lazily because the project's
// boot path can call extendTo() at any time.
static uint32_t _origTimeoutS = DEFAULT_WDT_TIMEOUT_S;
static bool     _extended     = false;

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
    esp_task_wdt_init(_origTimeoutS, false);
    _extended = false;
    Log.printf("[WDT] Restored to %us\n", (unsigned)_origTimeoutS);
}

}  // namespace wb_wdt
