#include "wb_health.h"
#include "wb_log.h"
#include "wb_ble.h"
#include "wb_mqtt.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <WiFi.h>
#include <ArduinoJson.h>

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
    // We don't yet know what uptime this firmware will reach — record 0
    // here, the periodic publishGatewayInfo path could update if we
    // wanted that, but as a one-shot at boot it's fine to leave 0.
    e["at"]     = (uint32_t)(millis() / 1000);
    while ((int)arr.size() > BOOT_HIST_MAX) arr.remove(0);
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
    Log.println("[Health] Marked healthy — boot counter cleared, OTA validated");
}

bool isHealthy() { return _healthy; }

bool canAcceptOta(String& reasonOut) {
    uint32_t up = millis();
    if (up < OTA_MIN_UPTIME_MS) {
        reasonOut = "uptime too low (need >" + String(OTA_MIN_UPTIME_MS / 1000) + "s, have " + String(up / 1000) + "s)";
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

}  // namespace wb_health
