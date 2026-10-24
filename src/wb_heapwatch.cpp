// See wb_heapwatch.h.
#include "wb_heapwatch.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "wb_health.h"

namespace wb_heapwatch {

// Own magic + block (not appended to the wb_health breadcrumbs) so adding this
// didn't change that struct's layout and discard its saved crash trace.
static const uint32_t HW_MAGIC = 0x48EA9501UL;

typedef struct {
    uint32_t magic;
    // Failed allocations this boot (any task, any caps).
    uint32_t fail_count;
    uint32_t fail_size;     // bytes asked for by the most recent failure
    uint32_t fail_caps;
    uint32_t fail_ms;       // millis() of the most recent failure
    uint32_t fail_free;     // free 8-bit heap at that instant
    uint32_t fail_largest;  // largest free 8-bit block at that instant
    char     fail_path[32];
    char     fail_bapi[16];
    // Lowest free heap seen by the main-loop sampler this boot.
    uint32_t low_min;
    uint32_t low_ms;
    char     low_path[32];
    char     low_bapi[16];
} __attribute__((packed)) HeapEvidence;

static RTC_NOINIT_ATTR HeapEvidence _ev;
static HeapEvidence _prev = {};
static bool         _prevValid = false;

static void _copy(char* dst, size_t cap, const char* src) {
    if (!src) src = "";
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Runs in the allocating task's context right after the failure. Must not
// allocate (the heap is out) and must stay short: plain word stores and
// strnlen/memcpy into the RTC block only.
static void _onAllocFailed(size_t size, uint32_t caps, const char* /*fn*/) {
    // Libraries probe for PSRAM and fall back to internal RAM; on a no-PSRAM
    // board those probes always "fail" and would drown the real signal.
    if (caps & MALLOC_CAP_SPIRAM) return;
    _ev.fail_count++;
    _ev.fail_size    = (uint32_t)size;
    _ev.fail_caps    = caps;
    _ev.fail_ms      = millis();
    _ev.fail_free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    _ev.fail_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    _copy(_ev.fail_path, sizeof(_ev.fail_path), wb_health::breadcrumbPath());
    _copy(_ev.fail_bapi, sizeof(_ev.fail_bapi), wb_health::breadcrumbBapi());
}

void begin() {
    if (_ev.magic == HW_MAGIC) {
        _prev = _ev;
        _prev.fail_path[sizeof(_prev.fail_path) - 1] = '\0';
        _prev.fail_bapi[sizeof(_prev.fail_bapi) - 1] = '\0';
        _prev.low_path[sizeof(_prev.low_path) - 1]   = '\0';
        _prev.low_bapi[sizeof(_prev.low_bapi) - 1]   = '\0';
        _prevValid = (_prev.fail_count > 0 || _prev.low_min > 0);
    }
    memset(&_ev, 0, sizeof(_ev));
    _ev.magic   = HW_MAGIC;
    _ev.low_min = UINT32_MAX;
    heap_caps_register_failed_alloc_callback(_onAllocFailed);
}

void tick() {
    uint32_t m = ESP.getMinFreeHeap();
    if (m >= _ev.low_min) return;
    // getMinFreeHeap() is the allocator's own watermark, so the dip itself may
    // have been between ticks; the breadcrumbs say what was in flight around it.
    _ev.low_min = m;
    _ev.low_ms  = millis();
    _copy(_ev.low_path, sizeof(_ev.low_path), wb_health::breadcrumbPath());
    _copy(_ev.low_bapi, sizeof(_ev.low_bapi), wb_health::breadcrumbBapi());
}

static void _fill(JsonObject o, const HeapEvidence& e) {
    JsonObject f = o["alloc_fail"].to<JsonObject>();
    f["n"] = e.fail_count;
    if (e.fail_count) {
        f["size"]    = e.fail_size;
        f["caps"]    = e.fail_caps;
        f["at_s"]    = e.fail_ms / 1000;
        f["free"]    = e.fail_free;
        f["largest"] = e.fail_largest;
        f["path"]    = e.fail_path;
        f["bapi"]    = e.fail_bapi;
    }
    if (e.low_min != UINT32_MAX && e.low_min > 0) {
        JsonObject l = o["heap_low"].to<JsonObject>();
        l["min"]  = e.low_min;
        l["at_s"] = e.low_ms / 1000;
        l["path"] = e.low_path;
        l["bapi"] = e.low_bapi;
    }
}

void toJson(JsonObject out) {
    _fill(out, _ev);
    if (_prevValid) _fill(out["prev"].to<JsonObject>(), _prev);
}

void addPreviousToBootEntry(JsonObject entry) {
    if (_prevValid) _fill(entry["hw"].to<JsonObject>(), _prev);
}

}  // namespace wb_heapwatch
