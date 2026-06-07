#include "wb_web_async.h"
#include "wb_config.h"
#include "wb_log.h"
#include "wb_version.h"
#include "wb_health.h"

#if WB_ASYNC_WEB

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Forward declarations of state exposed by sync wb_web.cpp that the
// async handlers need to read. Defined in wb_web.cpp (the `static`
// qualifier was dropped on the auth-lockout vars so the linker can
// resolve these externs across translation units).
extern volatile uint32_t g_loopMaxMs;
extern volatile int      g_webMaxReentry;
int wb_web_tokens_remaining();

// Sync server's auth lockout state. Sharing it between sync and
// async servers means a brute-forcer can't double their throughput
// by alternating ports. When the sync server retires post-migration,
// this re-homes into local state in this TU.
extern uint32_t authFailCount;
extern uint32_t authLockoutUntil;

namespace wb_web_async {

// Async-server instance. Port 8081 during migration; will move to
// port 80 when all 40 sync routes have been migrated and the legacy
// sync server is retired.
static AsyncWebServer _async(8081);

// --- Auth helper ---
//
// Mirrors wb_web.cpp::checkAuth() but operates on the
// AsyncWebServerRequest object instead of the global sync server
// state. Returns true if authorised; sends 401 + returns false
// otherwise.
static bool _checkAuth(AsyncWebServerRequest* req) {
    const WBConfig& cfg = configMgr.get();
    if (!cfg.authEnabled || cfg.authPass.length() == 0) return true;

    if (authFailCount >= 5 && millis() < authLockoutUntil) {
        req->send(429, "text/plain",
            "Too many attempts. Try again in 30 seconds.");
        return false;
    }
    if (millis() >= authLockoutUntil) authFailCount = 0;

    if (!req->authenticate(cfg.authUser.c_str(), cfg.authPass.c_str())) {
        authFailCount++;
        if (authFailCount >= 5) {
            authLockoutUntil = millis() + 30000;
            Log.printf("[Auth-async] LOCKED OUT after %d failures\n",
                       authFailCount);
        } else {
            Log.printf("[Auth-async] Failed %d/5\n", authFailCount);
        }
        req->requestAuthentication();
        return false;
    }
    authFailCount = 0;
    return true;
}

// =====================================================================
// First batch of migrated routes — read-only JSON endpoints.
// Each route reads from in-memory state (no BLE calls, no MQTT
// publishes, no NVS writes), so handler execution on the AsyncTCP
// task is straight-forward.
// =====================================================================

static void _registerReadOnlyRoutes() {

    // GET /api/health — cheap status snapshot. Matches the sync
    // server's payload bit-for-bit so external monitors can hit
    // either port and see the same thing during migration.
    _async.on("/api/health", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        JsonDocument doc;
        doc["ok"]             = true;
        doc["max_reentry"]    = g_webMaxReentry;
        doc["tokens"]         = wb_web_tokens_remaining();
        doc["loop_max_ms"]    = (uint32_t)g_loopMaxMs;
        doc["heap_free"]      = (uint32_t)ESP.getFreeHeap();
        doc["uptime"]         = (uint32_t)(millis() / 1000);
        doc["ota_proven"]     = wb_health::otaProven();
        doc["ota_min_uptime"] = wb_health::effectiveOtaMinUptimeMs() / 1000;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // GET /api/async/ping — async-stack proof of life. No auth.
    // Returns build version so a test can confirm the async server
    // is the SAME firmware (vs. a stale process from a prior boot).
    _async.on("/api/async/ping", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        String body = String("{\"ok\":true,\"fw\":\"")
                    + WB_VERSION
                    + "\",\"server\":\"async\"}";
        req->send(200, "application/json", body);
    });
}

void begin() {
    _registerReadOnlyRoutes();
    _async.begin();
    Log.println("[Web-async] Listening on :8081 (migration coexistence)");
}

}  // namespace wb_web_async

#else  // !WB_ASYNC_WEB

// Flag OFF: stub. begin() compiles to a no-op return; the async
// library symbols are unreferenced and dead-stripped by the linker.
namespace wb_web_async {
void begin() {}
}

#endif  // WB_ASYNC_WEB
