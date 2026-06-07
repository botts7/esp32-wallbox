#include "wb_web_async.h"
#include "wb_config.h"
#include "wb_log.h"
#include "wb_version.h"
#include "wb_health.h"
#include "wb_diag.h"
#include "wb_ota_history.h"
#include "wb_ble.h"
#include "wb_web.h"  // for webServer.requestReboot()

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

// 3.0 task #78: JSON builders extracted from sync handlers in
// wb_web.cpp so the async server emits IDENTICAL payloads without
// duplicating the field lists. See those functions for the per-field
// rationale (charger_sessions null encoding, cache-age semantics,
// etc.).
String wb_buildStatusJson();
String wb_buildChargerJson();
String wb_buildDiagRuntimeJson();
String wb_buildBootHistoryJson();

// Sync server's auth lockout state. Sharing it between sync and
// async servers means a brute-forcer can't double their throughput
// by alternating ports. When the sync server retires post-migration,
// this re-homes into local state in this TU.
extern uint32_t authFailCount;
extern uint32_t authLockoutUntil;

// CSRF state — also shared with sync server for the same reason.
extern String csrfToken;
extern bool   csrfTokenReady;
void ensureCsrfToken();

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

// --- CSRF helper for state-mutating endpoints ---
//
// Mirrors wb_web.cpp::checkCsrf() — reads the `csrf` query/form
// parameter and compares against the persisted token. Returns true
// if the token matches; sends 403 and returns false otherwise.
//
// Same shared-token rationale as auth state — running two CSRF
// secrets across the migration window is wasted complexity.
static bool _checkCsrf(AsyncWebServerRequest* req) {
    ensureCsrfToken();
    String token;
    if (req->hasParam("csrf"))         token = req->getParam("csrf")->value();
    else if (req->hasParam("csrf", true)) token = req->getParam("csrf", true)->value();  // POST form
    if (token.length() == 0 || token != csrfToken) {
        req->send(403, "text/plain", "CSRF token mismatch");
        return false;
    }
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

    // GET /api/status — large status snapshot for /info + the web UI's
    // diag panel.
    _async.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        req->send(200, "application/json", wb_buildStatusJson());
    });

    // GET /api/charger — cached status JSON for the dashboard. Never
    // blocks on BLE; the cache is refreshed by the main task's BLE
    // poll completion.
    _async.on("/api/charger", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        req->send(200, "application/json", wb_buildChargerJson());
    });

    // GET /api/diag/disconnects — counters + NVS-persisted ring of
    // BLE/MQTT/WiFi reconnect events.
    _async.on("/api/diag/disconnects", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/json", wb_diag::toJson());
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    // GET /api/diag/runtime — heap + per-task stack high-water marks.
    _async.on("/api/diag/runtime", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/json", wb_buildDiagRuntimeJson());
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    // GET /api/boot/history — last ~10 boot reasons.
    _async.on("/api/boot/history", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/json", wb_buildBootHistoryJson());
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    // GET /api/ota/history — newest-first list of OTA attempts.
    _async.on("/api/ota/history", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/json", wb_ota_history::toJson());
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    // GET /api/logs — last ~16 KB of serial / telnet output as plain
    // text. Used by /logs page and external monitors.
    _async.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        String body;
        Log.copyBuffer(body);
        AsyncWebServerResponse* res = req->beginResponse(200,
            "text/plain; charset=utf-8", body);
        res->addHeader("Cache-Control", "no-store");
        req->send(res);
    });

    // GET /api/command_status?id=N — poll endpoint for async BAPI
    // requests enqueued via /api/command?wait=0. Returns:
    //   200 + body  → response landed (consumed-on-read)
    //   202         → still in flight or evicted but possibly tracked
    //   410         → id was never issued by this gateway boot
    //   400         → missing or zero id
    // Same semantics as the sync handler in wb_web.cpp. Calls
    // wallboxBLE.tryFetchResponse / peekNextReqId which are both
    // already cross-task safe (mutex-protected response map + atomic
    // _nextReqId per the 2.7.0 audit fix).
    _async.on("/api/command_status", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!req->hasParam("id")) {
            req->send(400, "application/json", "{\"error\":\"missing id\"}");
            return;
        }
        uint32_t reqId = (uint32_t)req->getParam("id")->value().toInt();
        if (reqId == 0) {
            req->send(400, "application/json", "{\"error\":\"invalid id\"}");
            return;
        }
        String body;
        if (wallboxBLE.tryFetchResponse(reqId, body) && body.length()) {
            req->send(200, "application/json", body);
            return;
        }
        uint32_t nextId = wallboxBLE.peekNextReqId();
        if (reqId >= nextId) {
            req->send(410, "application/json",
                "{\"error\":\"unknown id — never issued\"}");
            return;
        }
        String pending = "{\"id\":" + String(reqId) + ",\"status\":\"pending\"}";
        req->send(202, "application/json", pending);
    });
}

// =====================================================================
// Static-content routes (sw.js, manifest.json, favicon) and small
// state-mutating endpoints (reboot, pin, fw/dismiss, diag/clear,
// ble/pause). These call into modules that are already cross-task
// safe — wallboxBLE has its own mutex, configMgr's Preferences/NVS
// API is thread-safe per ESP-IDF docs, webServer.requestReboot() is
// a single bool write.
// =====================================================================

static void _registerStaticAndPostRoutes() {

    // GET /sw.js — service worker. Static body.
    _async.on("/sw.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/javascript",
            "self.addEventListener('install',function(e){e.waitUntil(caches.keys().then(function(keys){return Promise.all(keys.map(function(k){return caches.delete(k)}))}).then(function(){return self.skipWaiting()}))});"
            "self.addEventListener('activate',function(e){e.waitUntil(self.clients.claim())});"
            "self.addEventListener('fetch',function(e){e.respondWith(fetch(e.request,{cache:'no-cache'}))});");
        res->addHeader("Cache-Control", "no-cache");
        req->send(res);
    });

    // GET /manifest.json — PWA manifest. Static body matching sync.
    _async.on("/manifest.json", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/manifest+json",
            "{\"name\":\"Wallbox Gateway\",\"short_name\":\"Wallbox\","
            "\"display\":\"standalone\",\"orientation\":\"portrait\","
            "\"background_color\":\"#0f1117\",\"theme_color\":\"#0f1117\","
            "\"start_url\":\"/\",\"scope\":\"/\","
            "\"icons\":[{"
            "\"src\":\"data:image/svg+xml;utf8,"
            "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 192 192'>"
            "<rect width='192' height='192' rx='40' fill='%230f1117'/>"
            "<path d='M104 36 L60 104 L92 104 L88 156 L132 88 L100 88 L104 36 Z' fill='%233b82f6'/>"
            "</svg>\","
            "\"sizes\":\"192x192\",\"type\":\"image/svg+xml\",\"purpose\":\"any\"}]}");
        res->addHeader("Cache-Control", "public, max-age=86400");
        req->send(res);
    });

    // GET /favicon.ico — return 204. Same as sync.
    _async.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(204);
    });

    // GET /api/ble/pause?ms=N — pause BLE for N milliseconds.
    // 5 min default, 30s minimum, 30 min maximum. wallboxBLE.pause()
    // is mutex-protected internally.
    _async.on("/api/ble/pause", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        uint32_t ms = 5 * 60 * 1000;
        if (req->hasParam("ms")) {
            ms = (uint32_t)req->getParam("ms")->value().toInt();
        }
        if (ms < 30000)            ms = 30000;
        if (ms > 30 * 60 * 1000)   ms = 30 * 60 * 1000;
        wallboxBLE.pause(ms);
        String body = String("{\"paused_for_s\":") + String(ms / 1000) + "}";
        req->send(200, "application/json", body);
    });

    // POST /api/fw/dismiss — clear the "firmware changed" banner.
    // Auth only; no CSRF (matches sync, which intentionally allows
    // a click-dismiss flow without form tokens).
    _async.on("/api/fw/dismiss", HTTP_POST,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        wallboxBLE.dismissFirmwareChange();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/diag/clear — wipe reconnect counters, smart-tripwire
    // ring, NVS-persisted event history. Auth + CSRF.
    _async.on("/api/diag/clear", HTTP_POST,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkCsrf(req)) return;
        wb_diag::clear();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/reboot — schedule a reboot via the main task. We don't
    // call ESP.restart() directly because the HTTP response needs to
    // flush first; webServer.requestReboot() flips a flag that the
    // sync server's loop() picks up after the current request.
    _async.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkCsrf(req)) return;
        req->send(200, "application/json",
            "{\"ok\":true,\"rebooting\":true}");
        webServer.requestReboot();
    });

    // POST /api/pin — update the stored BLE passcode (NVS) and reboot
    // so the new value takes effect on the next pair attempt. Sync
    // handler validates "digits only, ≤ 16 chars" — mirror here.
    _async.on("/api/pin", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkCsrf(req)) return;
        String pin;
        if (req->hasParam("pin"))            pin = req->getParam("pin")->value();
        else if (req->hasParam("pin", true)) pin = req->getParam("pin", true)->value();
        pin.trim();
        if (pin.length() > 16) {
            req->send(400, "application/json",
                "{\"error\":\"passcode too long\"}");
            return;
        }
        for (size_t i = 0; i < pin.length(); i++) {
            char c = pin[i];
            if (c < '0' || c > '9') {
                req->send(400, "application/json",
                    "{\"error\":\"passcode must be digits only\"}");
                return;
            }
        }
        configMgr.mut().blePin = pin;
        configMgr.save();
        req->send(200, "application/json",
            "{\"ok\":true,\"rebooting\":true}");
        webServer.requestReboot();
    });
}

void begin() {
    _registerReadOnlyRoutes();
    _registerStaticAndPostRoutes();
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
