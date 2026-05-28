#pragma once

#include <Arduino.h>

// Runtime health tracking — drives /api/health and the OTA admission guard.
//
// Boot counter: incremented at very early setup() and persisted to NVS.
// When the device reaches a healthy state (WiFi + MQTT both up for >30s)
// we reset the counter. If a fresh boot reads counter >= BOOT_FAIL_THRESHOLD
// before doing anything else, we force-revert to the other OTA partition
// (or just hold off on the new firmware) and reset the counter ourselves.
//
// This catches the case where a half-flashed firmware boots but never
// reaches a healthy state — without this, ESP32 happily keeps trying
// to boot the corrupt partition forever.

namespace wb_health {

// Read-and-increment the boot counter from NVS. Returns the new value.
// Call this very early in setup(), BEFORE WiFi or BLE init.
uint8_t bootCountBumpAndRead();

// Capture esp_reset_reason() of this boot, persist a short history of
// recent boots (reason + uptime-at-time-of-recording) to NVS. Call
// in setup() right after the boot-banner log. Stores up to 10 entries,
// newest first.
void recordBootReason();

// Get current boot's reset reason as a short string ("power-on",
// "panic", "task-wdt", "brownout", "external", "software", ...).
const char* currentBootReasonStr();

// Return the persisted history as a JSON array string (newest first).
// Each entry: {reason, uptime_was_s}.
String bootHistoryJson();

// Reset the boot counter to 0 (call when healthy).
void bootCountReset();

// Number of failed-to-be-healthy boots before forcing a rollback.
static const uint8_t BOOT_FAIL_THRESHOLD = 3;

// Called once when WiFi+MQTT are both up for >30s. Marks the current
// firmware as healthy (resets boot counter, marks OTA partition valid).
void markHealthy();

// Has the current boot reached a healthy state yet?
bool isHealthy();

// Minimum uptime (ms) before OTA upload is accepted.
// Prevents flash storms — gateway has to be settled before reflashing.
static const uint32_t OTA_MIN_UPTIME_MS = 60000;

// Returns true if it's safe to start a new OTA upload now.
// Caller should reply 503 with the reason if false.
bool canAcceptOta(String& reasonOut);

}  // namespace wb_health
