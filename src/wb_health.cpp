#include "wb_health.h"
#include "wb_log.h"
#include "wb_ble.h"
#include "wb_mqtt.h"
#include "wb_version.h"
#include "wb_ota_history.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <time.h>

namespace wb_health {

static bool _healthy = false;
static uint32_t _healthySinceMs = 0;
static const uint32_t HEALTHY_DWELL_MS = 30000;  // must be up 30s before marking valid
static const char* NVS_NS = "wbhealth";
static const char* NVS_KEY = "boot_count";

uint8_t bootCountBumpAndRead() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return 0;
    uint8_t n = p.getUChar(NVS_KEY, 0);
    n = (n < 255) ? n + 1 : n;
    p.putUChar(NVS_KEY, n);
    p.end();
    return n;
}

void bootCountReset() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putUChar(NVS_KEY, 0);
    p.end();
}

static const char* NVS_BOOT_HIST = "boot_hist";  // JSON array of recent boot reasons
static const uint8_t BOOT_HIST_MAX = 10;
static esp_reset_reason_t _thisBootReason = ESP_RST_UNKNOWN;

// Crash-trace breadcrumbs in RTC NOINIT memory. Survive panic +
// warm-boot; lost on cold power-on (acceptable: a cold start has no
// previous crash to attribute to). Magic verifies the struct wasn't
// trampled by an unrelated RTC NOINIT consumer.
static const uint32_t WB_BC_MAGIC = 0xB12EAD0FUL;  // "B12EAD0F" — breadcrumb tag

typedef struct {
    uint32_t magic;
    char     path[32];     // last HTTP request path (truncated)
    char     bapi[16];     // last BAPI met submitted to BLE worker
    uint32_t loop_count;   // main-loop tick counter
    uint8_t  reserved[4];
} __attribute__((packed)) WBBreadcrumbs;

static RTC_NOINIT_ATTR WBBreadcrumbs _bc;
// Cache the previous-boot snapshot so /api/boot/history can surface it.
static WBBreadcrumbs _bcSnapshot = {};
static bool          _bcSnapshotValid = false;

void setBreadcrumbPath(const char* path) {
    if (!path) return;
    size_t n = strnlen(path, sizeof(_bc.path) - 1);
    memcpy(_bc.path, path, n);
    _bc.path[n] = '\0';
}

void setBreadcrumbBapi(const char* met) {
    if (!met) return;
    size_t n = strnlen(met, sizeof(_bc.bapi) - 1);
    memcpy(_bc.bapi, met, n);
    _bc.bapi[n] = '\0';
}

void bumpBreadcrumbLoop() { _bc.loop_count++; }

const char* currentBootReasonStr() {
    switch (_thisBootReason) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external-reset";
        case ESP_RST_SW:        return "software (ESP.restart)";
        case ESP_RST_PANIC:     return "panic (crash)";
        case ESP_RST_INT_WDT:   return "interrupt-watchdog";
        case ESP_RST_TASK_WDT:  return "task-watchdog";
        case ESP_RST_WDT:       return "watchdog (other)";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

void recordBootReason() {
    _thisBootReason = esp_reset_reason();
    Log.printf("[Health] Boot reset reason: %s\n", currentBootReasonStr());

    // Snapshot the RTC NOINIT breadcrumbs from the previous boot BEFORE
    // we overwrite them with this boot's identity. If the magic matches,
    // the previous boot left a valid trail. Surface only on crash-class
    // reasons so a clean software-restart doesn't blame the last action.
    bool crashClass = (_thisBootReason == ESP_RST_PANIC ||
                       _thisBootReason == ESP_RST_INT_WDT ||
                       _thisBootReason == ESP_RST_TASK_WDT ||
                       _thisBootReason == ESP_RST_WDT ||
                       _thisBootReason == ESP_RST_BROWNOUT);
    if (_bc.magic == WB_BC_MAGIC && crashClass) {
        _bcSnapshot = _bc;
        _bcSnapshotValid = true;
        // Defensive null-termination — the strings should already be
        // terminated, but RTC RAM can corrupt under brownout.
        _bcSnapshot.path[sizeof(_bcSnapshot.path) - 1] = '\0';
        _bcSnapshot.bapi[sizeof(_bcSnapshot.bapi) - 1] = '\0';
        Log.printf("[Health] Previous-boot breadcrumbs — last path: '%s'  last BAPI: '%s'  loop count: %u\n",
                   _bcSnapshot.path, _bcSnapshot.bapi, (unsigned)_bcSnapshot.loop_count);
    }
    // Re-arm the breadcrumbs for THIS boot.
    memset(&_bc, 0, sizeof(_bc));
    _bc.magic = WB_BC_MAGIC;

    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    String s = p.getString(NVS_BOOT_HIST, "[]");
    JsonDocument doc;
    if (deserializeJson(doc, s) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        doc.clear();
        doc.to<JsonArray>();
    }
    JsonArray arr = doc.as<JsonArray>();
    JsonObject e = arr.add<JsonObject>();
    e["reason"] = currentBootReasonStr();
    e["raw"]    = (int)_thisBootReason;
    if (_bcSnapshotValid) {
        // Persist the breadcrumb trail next to the boot reason so /info
        // can show "panic at last path X, last BAPI Y" without a serial
        // console. Truncated to fit comfortably in the NVS slot.
        JsonObject bc = e["bc"].to<JsonObject>();
        bc["path"]       = _bcSnapshot.path;
        bc["bapi"]       = _bcSnapshot.bapi;
        bc["loop_count"] = _bcSnapshot.loop_count;
    }
    // `at` is wall-clock epoch when first observed by SNTP. NTP hasn't
    // synced yet at recordBootReason() time (very early setup), so seed
    // 0 here. updateBootTimeIfPossible() patches it once NTP comes up.
    e["at"]     = 0;
    // `fw` lets /info distinguish "panics on the CURRENT firmware
    // version" from "panics carried over from older dev firmware in the
    // NVS ring". Without this, every fresh release would inherit the
    // bad-boot count of whatever was being debugged on the previous
    // firmware. Entries written before rc21 have no `fw` field and are
    // treated by the badge as "prior firmware".
    e["fw"]     = WB_VERSION;
    while ((int)arr.size() > BOOT_HIST_MAX) arr.remove(0);
    String out;
    serializeJson(doc, out);
    p.putString(NVS_BOOT_HIST, out);
    p.end();
}

void updateBootTimeIfPossible() {
    time_t now = time(nullptr);
    // Pre-2024 means SNTP hasn't replaced the boot-default 1970 yet.
    // Threshold is generous on purpose — we don't need a precise cutoff,
    // just "obviously after a real NTP sync".
    if (now < 1704067200) return;  // 2024-01-01 00:00:00 UTC

    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    String s = p.getString(NVS_BOOT_HIST, "[]");
    JsonDocument doc;
    if (deserializeJson(doc, s) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        p.end();
        return;
    }
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) { p.end(); return; }
    JsonObject last = arr[arr.size() - 1];
    if (last["at"].as<uint32_t>() != 0) { p.end(); return; }  // already patched
    last["at"] = (uint32_t)now;
    String out;
    serializeJson(doc, out);
    p.putString(NVS_BOOT_HIST, out);
    p.end();
}

String bootHistoryJson() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return "[]";
    String s = p.getString(NVS_BOOT_HIST, "[]");
    p.end();
    // Reverse so newest is first
    JsonDocument doc;
    if (deserializeJson(doc, s) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        return "[]";
    }
    JsonArray arr = doc.as<JsonArray>();
    JsonDocument out;
    JsonArray oa = out.to<JsonArray>();
    for (int i = (int)arr.size() - 1; i >= 0; i--) oa.add(arr[i]);
    String result;
    serializeJson(out, result);
    return result;
}

void markHealthy() {
    if (_healthy) return;
    _healthy = true;
    bootCountReset();
    // Tell the ESP OTA layer to NOT roll back to the previous partition
    // on next boot. Without this, if rollback is enabled in sdkconfig,
    // the partition would revert on the next reboot. Safe to call even
    // if rollback isn't enabled — it's a no-op then.
    esp_ota_mark_app_valid_cancel_rollback();
    // Record that THIS firmware reached healthy state. Pairs with the
    // wb_ota_history `ota` entries (written by the *previous* firmware
    // at upload time) to give /info a complete chronological story of
    // "what got installed and which ones actually booted ok". Without
    // this half, the history shows uploads but not which versions ran
    // successfully — Peter's 2.4.2 follow-up.
    wb_ota_history::recordBoot(millis() / 1000, WB_VERSION);
    Log.println("[Health] Marked healthy — boot counter cleared, OTA validated, boot recorded");
}

bool isHealthy() { return _healthy; }

// "OTA-proven" flag in NVS. Set the first time an OTA commits successfully
// AND the new firmware reaches healthy state. Once set, future OTAs use
// the relaxed uptime window — the device has demonstrated it can survive
// a flash cycle, so the conservative 60s settling window isn't needed.
static const char* NVS_OTA_PROVEN = "ota_proven";
static int8_t _otaProvenCache = -1;  // -1=unread, 0=false, 1=true

bool otaProven() {
    if (_otaProvenCache >= 0) return _otaProvenCache == 1;
    Preferences p;
    if (!p.begin(NVS_NS, true)) { _otaProvenCache = 0; return false; }
    uint8_t v = p.getUChar(NVS_OTA_PROVEN, 0);
    p.end();
    _otaProvenCache = v ? 1 : 0;
    return v != 0;
}

void markOtaSuccess() {
    if (otaProven()) return;
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putUChar(NVS_OTA_PROVEN, 1);
    p.end();
    _otaProvenCache = 1;
    Log.println("[Health] OTA-proven flag set — future OTAs use relaxed admission window");
}

uint32_t effectiveOtaMinUptimeMs() {
    return otaProven() ? OTA_MIN_UPTIME_PROVEN_MS : OTA_MIN_UPTIME_MS;
}

bool canAcceptOta(String& reasonOut) {
    uint32_t up = millis();
    uint32_t need = effectiveOtaMinUptimeMs();
    if (up < need) {
        reasonOut = "uptime too low (need >" + String(need / 1000) + "s, have " + String(up / 1000) + "s)";
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        reasonOut = "WiFi not connected";
        return false;
    }
    if (!_healthy) {
        reasonOut = "gateway not yet marked healthy (BLE/MQTT still settling)";
        return false;
    }
    return true;
}

uint32_t otaRetryAfterSeconds() {
    uint32_t up = millis();
    uint32_t need = effectiveOtaMinUptimeMs();
    if (up < need) {
        // Caller should wait until we cross the threshold, plus a small
        // cushion so the next admission check definitely passes.
        uint32_t remainSec = (need - up + 999) / 1000;
        return remainSec + 2;
    }
    // Other rejection reasons (WiFi, healthy gate) are transient and
    // typically self-heal within a few seconds — short retry is fine.
    return 5;
}

}  // namespace wb_health
