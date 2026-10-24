#pragma once
// Heap evidence that survives a crash.
//
// The 2026-10-18 panic (LoadProhibited at vaddr 0 in String::move under
// AsyncBasicResponse::_respond) was a failed ~300-byte allocation: Arduino's
// String leaves a null buffer when operator+/substring can't allocate, and the
// following move reads address 0. heap_min_ever only says THAT the heap got
// low, never WHEN or doing WHAT. This module records both:
//
//   - alloc_fail: IDF's failed-allocation hook. Count, the size asked for,
//     free heap + largest block at that instant, and the in-flight HTTP path /
//     BAPI met (from the wb_health breadcrumbs).
//   - heap_low:   each new lifetime-minimum free heap, with the same context,
//     sampled from the main loop.
//
// Both live in RTC NOINIT memory, so after a panic the previous boot's
// evidence is attached to its boot-history entry and shown in /api/health.

#include <ArduinoJson.h>

namespace wb_heapwatch {

// Call once, early in setup() and BEFORE wb_health::recordBootReason() re-arms
// its breadcrumbs: snapshots the previous boot's evidence, re-arms the RTC
// block and registers the failed-alloc hook.
void begin();

// Main-loop tick: records a new heap low (cheap: one ESP.getMinFreeHeap()).
void tick();

// /api/health: {"alloc_fail":{...},"heap_low":{...}} for this boot, plus
// "prev" when the previous boot left evidence.
void toJson(JsonObject out);

// Boot-history entry for a crash-class reset: attaches the previous boot's
// alloc_fail / heap_low so a panic can be read without a serial console.
void addPreviousToBootEntry(JsonObject entry);

}  // namespace wb_heapwatch
