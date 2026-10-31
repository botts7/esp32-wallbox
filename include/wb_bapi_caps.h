#pragma once
// Which periodic BAPI reads this charger doesn't support.
//
// Older chargers answer some reads with a BAPI error every time (the original
// Pulsar rejects r_dca, r_lse, r_sta, g_ecos, g_psh, g_phsw, g_tzn and
// g_halocfg, #68). Asking anyway costs a full exchange per read on every poll
// cycle, which on that charger's 23-byte MTU is several fragmented writes each.
//
// A read is marked unsupported after UNSUPPORTED_AFTER consecutive error
// replies with no success in between; a timeout (empty reply) is not an
// answer and changes nothing. Unsupported reads are skipped, except for one
// retry every RETRY_MS so a charger firmware update or a newly fitted
// accessory is picked up. Everything resets on each new BLE connection.
//
// Called from the BLE task only (shouldSkip / record / reset); toJson is read
// from the web task, where a momentarily stale view is harmless.

#include <Arduino.h>
#include <ArduinoJson.h>

namespace wb_bapi_caps {

static const uint8_t  UNSUPPORTED_AFTER = 3;
static const uint32_t RETRY_MS = 30UL * 60UL * 1000UL;

// Forget everything (new connection, possibly a different charger).
void reset();

// True when `met` is known unsupported and its retry isn't due yet.
bool shouldSkip(const char* met);

// Feed the reply to a read of `met`.
void record(const char* met, const String& resp);

// True when `met` is currently marked unsupported.
bool unsupported(const char* met);

// /api/status: array of the methods currently marked unsupported.
void toJson(JsonArray out);

}  // namespace wb_bapi_caps
