#include "wb_control.h"

namespace wb_control {

// Tiny shared state touched from the AsyncTCP task (command handler) and read
// from whichever task builds /api/status. A portMUX critical section guards
// the fixed buffer; Strings are built OUTSIDE the section (no alloc under the
// spinlock).
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
static char _by[24] = {0};
static uint32_t _ms = 0;

void recordCommand(const String& owner) {
    const char* o = owner.length() ? owner.c_str() : "manual";
    uint32_t now = millis();
    portENTER_CRITICAL(&_mux);
    strncpy(_by, o, sizeof(_by) - 1);
    _by[sizeof(_by) - 1] = 0;
    _ms = now;
    portEXIT_CRITICAL(&_mux);
}

String lastCommandBy() {
    char buf[24];
    portENTER_CRITICAL(&_mux);
    memcpy(buf, _by, sizeof(buf));
    portEXIT_CRITICAL(&_mux);
    return String(buf);
}

uint32_t lastCommandAgeMs() {
    uint32_t now = millis();
    portENTER_CRITICAL(&_mux);
    uint32_t age = _by[0] ? (now - _ms) : 0xFFFFFFFFUL;
    portEXIT_CRITICAL(&_mux);
    return age;
}

}  // namespace wb_control
