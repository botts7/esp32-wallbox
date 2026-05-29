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
// Each entry: {reason, raw, at, fw}. `at` is the wall-clock epoch when
// the boot was first observed by SNTP (0 if NTP hadn't synced yet);
// `fw` is the firmware version that recorded the entry. Both fields
// let the /info badge distinguish "panics on THIS firmware version,
// since a known time" from "old dev-testing panics carried over in the
// NVS ring".
String bootHistoryJson();

// Patch the most-recent boot entry's `at` field with the current
// wall-clock time. No-op if (a) NTP hasn't synced yet (`time(NULL)`
// returns a pre-2024 value), or (b) the current entry already has a
// non-zero `at`. Cheap to call from the main loop on a slow cadence.
void updateBootTimeIfPossible();

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
