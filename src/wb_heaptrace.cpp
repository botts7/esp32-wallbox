#include "wb_heaptrace.h"
#include "wb_ble.h"
#include "wb_diag.h"
#include <esp_heap_caps.h>

namespace wb_heaptrace {

struct Sample {
    uint32_t t_s;         // uptime seconds at sample
    uint32_t heap_free;   // total free internal (8-bit) heap
    uint32_t heap_largest;// largest contiguous free block
    uint32_t ble_age_s;   // seconds since last BLE activity
    uint16_t ble_recon;   // cumulative BLE reconnects
    uint8_t  ble_conn;    // 1 = BLE connected
};

static Sample   _ring[TRACE_LEN];
static int      _head  = 0;    // next write index
static int      _count = 0;    // valid samples
static uint32_t _lastMs = 0;

void begin() {
    _head = 0; _count = 0; _lastMs = 0;
}

void tick() {
    uint32_t now = millis();
    if (_lastMs != 0 && (now - _lastMs) < 1000) return;   // 1 Hz
    _lastMs = now;
    Sample& s = _ring[_head];
    s.t_s          = now / 1000;
    s.heap_free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s.heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s.ble_age_s    = wallboxBLE.lastActivityAge() / 1000;
    s.ble_recon    = (uint16_t)wb_diag::bleReconnects();
    s.ble_conn     = wallboxBLE.isConnected() ? 1 : 0;
    _head = (_head + 1) % TRACE_LEN;
    if (_count < TRACE_LEN) _count++;
}

String toJson() {
    String out;
    out.reserve((size_t)_count * 72 + 64);
    out += "{\"len\":";
    out += _count;
    out += ",\"samples\":[";
    // Emit oldest-first.
    int start = (_count < TRACE_LEN) ? 0 : _head;
    for (int i = 0; i < _count; i++) {
        const Sample& s = _ring[(start + i) % TRACE_LEN];
        if (i) out += ',';
        out += "{\"t\":";     out += s.t_s;
        out += ",\"f\":";     out += s.heap_free;
        out += ",\"lg\":";    out += s.heap_largest;
        out += ",\"ba\":";    out += s.ble_age_s;
        out += ",\"br\":";    out += s.ble_recon;
        out += ",\"bc\":";    out += s.ble_conn;
        out += "}";
    }
    out += "]}";
    return out;
}

} // namespace wb_heaptrace
