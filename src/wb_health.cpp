#include "wb_health.h"
#include "wb_log.h"
#include "wb_ble.h"
#include "wb_mqtt.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <WiFi.h>

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
