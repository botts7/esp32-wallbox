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

// Crash-trace breadcrumbs. Three small string fields + a loop counter
// live in RTC NOINIT memory: not initialised on warm reboot, so they
// survive a panic. Update at the entry of HTTP handlers, BLE TX, and
// each main-loop tick; recordBootReason() captures the snapshot on the
// next boot when the prior reset was a panic/watchdog.
void setBreadcrumbPath(const char* path);
void setBreadcrumbBapi(const char* met);
void bumpBreadcrumbLoop();

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

// Minimum uptime (ms) before OTA upload is accepted on a *fresh*
// device. Once the device has completed at least one OTA + reached
// healthy state on the new firmware, we drop to OTA_MIN_UPTIME_PROVEN_MS
// so the next user-driven OTA doesn't wait a full minute.
static const uint32_t OTA_MIN_UPTIME_MS        = 60000;
static const uint32_t OTA_MIN_UPTIME_PROVEN_MS = 15000;

// Returns the effective min-uptime threshold for THIS device (fresh or
// proven). Exposed for /api/health to surface in diagnostics.
uint32_t effectiveOtaMinUptimeMs();

// Has this device previously completed an OTA and reached healthy state
// on the new firmware? Persisted in NVS — survives reboots.
bool otaProven();

// Set by the OTA path just before ESP.restart() on a successful commit.
// Subsequent canAcceptOta() calls use the relaxed window.
void markOtaSuccess();

// Returns true if it's safe to start a new OTA upload now.
// Caller should reply 503 with the reason if false.
bool canAcceptOta(String& reasonOut);

// If admission failed, how many seconds the caller should wait before
// retrying. Used to populate the Retry-After header.
uint32_t otaRetryAfterSeconds();

}  // namespace wb_health
