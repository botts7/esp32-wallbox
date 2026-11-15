#include "wb_tz.h"

#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "wb_log.h"
#include "wb_tz_table.h"

namespace wb_tz {

static SemaphoreHandle_t s_mutex = nullptr;
static StaticSemaphore_t s_mutexBuf;
static const char* s_posix = nullptr;   // points into kWbTzTable
static char s_iana[48] = "";            // last name passed to setZone()

static const char* lookup(const char* iana) {
    unsigned lo = 0, hi = kWbTzTableCount;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(iana, kWbTzTable[mid].name);
        if (c == 0) return kWbTzTable[mid].posix;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}

// TZ is process-global and other code (configTime) may reset it, so make
// sure it holds our string right before converting. Caller holds s_mutex.
static void applyLocked() {
    const char* want = s_posix ? s_posix : "UTC0";
    const char* cur = getenv("TZ");
    if (!cur || strcmp(cur, want) != 0) {
        setenv("TZ", want, 1);
        tzset();
    }
}

void begin() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutexBuf);
}

bool setZone(const char* iana) {
    if (!iana) iana = "";
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = strncmp(iana, s_iana, sizeof(s_iana)) != 0;
    if (changed) {
        strncpy(s_iana, iana, sizeof(s_iana) - 1);
        s_iana[sizeof(s_iana) - 1] = '\0';
        s_posix = lookup(iana);
        applyLocked();
    }
    bool known = s_posix != nullptr;
    xSemaphoreGive(s_mutex);
    if (changed) {
        if (known) Log.printf("[TZ] Charger timezone %s -> %s\n", iana, s_posix);
        else       Log.printf("[TZ] Charger timezone '%s' not in table, using UTC\n", iana);
    }
    return known;
}

void localTime(time_t t, struct tm* out) {
    if (!s_mutex || !s_posix) { gmtime_r(&t, out); return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    applyLocked();
    localtime_r(&t, out);
    xSemaphoreGive(s_mutex);
}

}  // namespace wb_tz
