// See wb_lock_hold.h.
#include "wb_lock_hold.h"

#include <Preferences.h>

namespace wb_lock_hold {

static const char* kNs = "wblock";
static const char* kKey = "orig";
static int s_cached = -2;  // -2 = not read from NVS yet

int heldOriginal() {
    if (s_cached != -2) return s_cached;
    Preferences p;
    s_cached = p.begin(kNs, true) ? p.getInt(kKey, -1) : -1;
    p.end();
    return s_cached;
}

void markHeld(int originalSecs) {
    if (originalSecs < 0) originalSecs = 0;
    if (heldOriginal() == originalSecs) return;
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putInt(kKey, originalSecs);
    p.end();
    s_cached = originalSecs;
}

void clear() {
    if (heldOriginal() == -1) return;
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.remove(kKey);
    p.end();
    s_cached = -1;
}

}  // namespace wb_lock_hold
