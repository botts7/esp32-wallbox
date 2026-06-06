#pragma once

#include <Arduino.h>

// Async WiFi management — owns the initial connect, the event-driven
// state machine, and the explicit `WiFi.reconnect()` defer policy.
//
// Pre-2.7.0 the main loop ran a `checkWiFi()` every 30 s that called
// `WiFi.reconnect()` synchronously whenever `WiFi.status() != WL_CONNECTED`.
// That sync call could block the main task for several seconds during
// driver scan / auth / DHCP, surfacing as a loop_max_ms spike and
// pausing MQTT publish / WS push during the same window.
//
// New model:
//   1. The Arduino-ESP32 framework's WiFi driver event task notifies us
//      via `WiFi.onEvent()` on `GOT_IP`, `STA_DISCONNECTED`, `STA_LOST_IP`.
//      Handlers ONLY set flags / timestamps — no real work.
//   2. Main loop calls `wb_net::tick()` which drives the deferred work:
//      mDNS refresh, diag bookkeeping, AND the explicit `WiFi.reconnect()`
//      call ONLY if the driver's own auto-reconnect has clearly given up
//      (≥ 60 s since last GOT_IP) with a 1/2/5/15/60 s backoff ladder.
//
// Most transient flaps recover via the driver's auto-reconnect before
// our 60 s gate elapses — we never explicitly call WiFi.reconnect().
//
// Synchronization: WiFi event callback runs on the driver's event task
// (separate FreeRTOS task in esp_wifi, typically core 0). Main loop on
// core 1. Shared state is single-word volatile (atomic on Xtensa);
// event handlers only set flags — all real work deferred to `tick()`.

namespace wb_net {

// Initial blocking connect (~20 s timeout) — call from setup() after
// configMgr is ready. Returns true if connected; false on timeout
// (caller should fall through to AP-mode portal as before).
// Also registers the WiFi.onEvent handlers.
bool begin();

// Cached connected state. Cheap (volatile read). Use this on hot paths
// instead of `WiFi.status()` to avoid the driver's internal mutex.
bool isConnected();

// Per-loop tick: drains pending state changes (mDNS refresh on
// reconnect, diag reportDisconnect / reportReconnect), and invokes
// the backoff-gated explicit WiFi.reconnect() only when needed.
// Cheap (~10 us) when nothing's pending.
void tick();

// Counters / diag (mirrors wb_diag, surfaced for /api/status and HA).
uint32_t lastConnectedAtMs();    // millis() of last GOT_IP, 0 if never
uint32_t lastDisconnectedAtMs(); // millis() of last STA_DISCONNECTED, 0 if never

// Force an immediate explicit reconnect (skips the 60 s gate but
// still honours the backoff ladder). Currently unused; reserved for
// a future "Reconnect WiFi" button in the UI.
void forceReconnect();

}  // namespace wb_net
