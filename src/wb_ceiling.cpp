// See wb_ceiling.h.
#include "wb_ceiling.h"

#include <Preferences.h>

#include "wb_log.h"

namespace wb_ceiling {

static const char* kNs = "wbceil";
static const char* kKey = "max";

int load() {
    Preferences p;
    if (!p.begin(kNs, true)) return -1;
    const int v = p.getInt(kKey, -1);
    p.end();
    return (v >= 6 && v <= 80) ? v : -1;
}

void save(int amps) {
    if (amps < 6 || amps > 80) return;
    if (load() == amps) return;
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putInt(kKey, amps);
    p.end();
    Log.printf("[BLE] Current ceiling %d A saved\n", amps);
}

}  // namespace wb_ceiling
