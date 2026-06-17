#include "wb_web_async.h"
#include "wb_config.h"
#include "wb_log.h"
#include "wb_version.h"
#include "wb_health.h"
#include "wb_diag.h"
#include "wb_ota_history.h"
#include "wb_ble.h"
#include "wb_web.h"  // for webServer.requestReboot()
#include "bapi.h"     // for MET_* constants used in /api/command

#if WB_ASYNC_WEB

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>  // TWDT feed during long xTaskNotifyWait slices
#include "_gen_settings_body_gz.h"  // pre-gzipped /settings body (task #75)
#include "_gen_info_body_gz.h"      // pre-gzipped /info body (v3.1 #103)
#include "_gen_dashboard_body_gz.h" // pre-gzipped "/" dashboard body (v3.1 #103)
#include "_gen_sessions_body_gz.h"  // pre-gzipped /sessions body (v3.1 #103)
#include "wb_health.h"     // OTA admission guard (step I)
#include "wb_ws.h"         // AsyncWebSocket handler (#82 migration)
#include "wb_watchdog.h"   // wb_wdt::extendTo/restore for OTA flash erase
#include "wb_ota_history.h" // recordOta() for the OTA history badge
#include "wb_version.h"    // WB_VERSION for OTA history records
#include <Update.h>         // ESP32 Arduino OTA write API
#include <esp_ota_ops.h>    // esp_ota_get_next_update_partition for size check
#include <esp_heap_caps.h>  // heap_caps_get_largest_free_block for /api/health
#include <WiFi.h>
#include <functional>
#include <memory>

// File-scope (global namespace) externs for the shared CSS/JS
// literals defined in wb_web.cpp. MUST live outside namespace
// wb_web_async — otherwise the linker mangles them as
// wb_web_async::wb_getStyleCssLiteral() and fails to resolve.
extern const char* wb_getStyleCssLiteral();
extern const char* wb_getAppJsLiteral();
extern String wb_buildConfigExportJson();

// Forward declarations of state exposed by sync wb_web.cpp that the
// async handlers need to read. Defined in wb_web.cpp (the `static`
// qualifier was dropped on the auth-lockout vars so the linker can
// resolve these externs across translation units).
extern volatile uint32_t g_loopMaxMs;
extern volatile int      g_webMaxReentry;
int wb_web_tokens_remaining();

// OTA shared state — defined in wb_web.cpp. Sharing prevents a
// concurrent upload on the other port from sneaking past the
// otaInProgress guard, and lets the rejection retry-after counter
// roll over consistently.
extern bool     otaInProgress;
extern size_t   expectedOtaSize;
extern uint16_t otaRetryAfterSec;
extern String   otaRejectReason;

// 3.0 task #78: JSON builders extracted from sync handlers in
// wb_web.cpp so the async server emits IDENTICAL payloads without
// duplicating the field lists. See those functions for the per-field
// rationale (charger_sessions null encoding, cache-age semantics,
// etc.).
String wb_buildStatusJson();
String wb_buildChargerJson();
String wb_buildDiagRuntimeJson();
String wb_buildBootHistoryJson();
// HTML-page builders extracted from the sync handlers in wb_web.cpp.
String wb_buildDashboardPage();
String wb_buildConfigPage();
String wb_buildSetupPage();
String wb_buildInfoPage();
String wb_buildSessionsPage();
String wb_buildOtaPage();
String wb_buildLogsPage();
String wb_buildSettingsPage();
// Form-POST helpers (3.0 task #78 step E):
String wb_applySaveForm(std::function<String(const char*)> getArg);
String wb_applyResetAndPage();
// BLE-passthrough helpers (3.0 task #78 step F):
String wb_runBleScan();
struct ScanResult { int status; String body; };
ScanResult wb_runWifiScan();
// JSON-body POST helper (3.0 task #78 step G — config import):
struct ImportResult { int status; String body; bool reboot; };
ImportResult wb_applyConfigImport(const String& jsonBody);
bool tbAllow();

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
// 3.0 task #78 step J — port swap. Async is now the production
// server on port 80. The sync WebServer in wb_web.cpp moved to
// port 81 as a fallback during 3.0 testing; once the async path
// has been exercised across both botts7+peter-mcc rigs the sync
// server retires entirely (next commit's territory).
static AsyncWebServer _async(80);

// --- Heap-pressure admission guard ---
//
// Big HTML pages reserve 16-40 KB up-front to avoid grow-and-copy
// heap fragmentation (see project_esp32_oom_pattern.md). When the
// heap's largest contiguous block is below the page's reservation,
// the reserve() silently keeps the smaller capacity and += falls
// back to many small allocations — which under concurrent BLE
// response parsing can OOM panic. To prevent that, refuse the
// request with 503 + Retry-After before we even attempt the build.
//
// Heap-pressure crashes captured via crash-trace breadcrumbs on
// 2026-06-09: path=/info, bapi=r_dca, multiple panics. Pre-reserve
// alone wasn't enough under sustained navigation; the guard backs
// pressure off to the client.
// Smart busy page: actively polls /api/health for heap recovery and
// only reloads when the gateway has the contiguous block this page
// needs. Surfaces progress (current vs needed kB) so the user sees
// it actually working. All-static so the page itself never touches
// the heap. The "need" query-param is set by the guard.
static const char HEAP_BUSY_HTML[] PROGMEM =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Gateway busy</title>"
"<style>"
"html,body{margin:0;height:100%;background:#0a0e14;color:#e5e7eb;"
"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
"display:flex;align-items:center;justify-content:center}"
".card{text-align:center;padding:24px;max-width:340px}"
".sp{width:36px;height:36px;border:3px solid rgba(59,130,246,.25);"
"border-top-color:#3b82f6;border-radius:50%;margin:0 auto 14px;"
"animation:s 1s linear infinite}"
"@keyframes s{to{transform:rotate(360deg)}}"
"h1{font-size:17px;margin:0 0 6px;font-weight:600}"
"p{font-size:13px;color:#9ca3af;margin:4px 0}"
".meter{font-size:11px;color:#6b7280;margin-top:10px;font-variant-numeric:tabular-nums}"
"</style></head><body><div class='card'>"
"<div class='sp'></div>"
"<h1>Gateway busy</h1>"
"<p>Loading… will continue automatically.</p>"
"<p class='meter' id='m'>Checking memory&hellip;</p>"
"</div><script>"
"// Poll /api/health and reload only when the gateway has the"
"// contiguous heap block needed for the page that was 503'd. 40 KB"
"// is the worst-case (/info); smaller pages will succeed sooner"
"// from the server side once heap recovers."
"var need=40000;"
"var target=location.href;"
"var tries=0;"
"function tick(){"
"  tries++;"
"  fetch('/api/health',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){"
"    var lg=d.heap_largest||0;"
"    document.getElementById('m').textContent='Free contiguous '+(lg/1024).toFixed(0)+' KB / need '+(need/1024).toFixed(0)+' KB';"
"    if(lg>=need){location.replace(target)}"
"    else{setTimeout(tick, tries<5?1500:3000)}"
"  }).catch(function(){setTimeout(tick,3000)});"
"}"
"setTimeout(tick,800);"
"</script></body></html>";

static bool _checkHeapHeadroom(AsyncWebServerRequest* req, size_t need) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest >= need) return true;
    Log.printf("[Web] heap pressure: largest=%u needed=%u — 503 %s\n",
               (unsigned)largest, (unsigned)need, req->url().c_str());
    // Redirect to the same path with ?need=N so the busy page knows
    // both where to come back to and what threshold to wait for.
    // 303 keeps the URL semantically idempotent and prevents browsers
    // re-submitting any form data the user was working with.
    String target = req->url() + "?need=" + String((uint32_t)need);
    AsyncWebServerResponse* res = req->beginResponse_P(503, "text/html",
        (const uint8_t*)HEAP_BUSY_HTML, strlen_P(HEAP_BUSY_HTML));
    res->addHeader("Retry-After", "3");
    // The busy page reads need= from query; the path it polls back
    // to is location.pathname which loses the query, so the loop
    // converges: 503 -> busy page -> heap recovers -> reload original.
    (void)target;
    req->send(res);
    return false;
}

// --- Large-HTML-page send helper ---
//
// req->send(200, "text/html", bigString) forwards body.c_str() to the
// const char* overload, which builds an AsyncBasicResponse that COPIES
// the whole string into its own buffer. That means two ~40 KB blocks
// must be live at once. On the fragmented heap the second allocation
// fails for the largest pages (/info 34 KB, /sessions 35 KB) and the
// response goes out with Content-Length 0 — a silent failure (observed
// v3.0.0: builder returns 34429 bytes, client receives 0).
//
// Instead we hand ownership of the already-built String to a shared_ptr
// captured by a pull-based filler. AsyncTCP memcpy's slices out on
// demand; no second full-size allocation ever happens. Pull-based, so
// unlike AsyncResponseStream's push cbuf (which tripped the task WDT
// under load — see task #103) the filler just copies from a complete
// buffer and returns immediately. WDT-safe by construction.
static void _sendHtmlPage(AsyncWebServerRequest* req, String&& html) {
    auto body = std::make_shared<String>(std::move(html));
    const size_t len = body->length();
    AsyncWebServerResponse* res = req->beginResponse("text/html", len,
        [body](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
            size_t remaining = body->length() - index;
            size_t n = remaining < maxLen ? remaining : maxLen;
            if (n) memcpy(buf, body->c_str() + index, n);
            return n;
        });
    // Never cache the HTML pages. Without this the browser/PWA/service-worker
    // heuristically caches the page and serves stale JS after an OTA — which
    // is how a user kept hitting the old schedule-delete (clr_sch null) on a
    // gateway already updated to the fixed firmware. The page JS itself is
    // tiny relative to the gzipped /settings body; no-store costs nothing
    // here and guarantees a fresh page every load.
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
}

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
        // Largest contiguous free block — drops below heap_free under
        // fragmentation, which is what triggers OOM panics for the
        // big page builders even when total free heap looks fine.
        // Aim to keep this above ~40 KB; below 20 KB is danger zone.
        doc["heap_largest"]   = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        // Lifetime minimum — the absolute worst-case dip since boot.
        doc["heap_min_ever"]  = (uint32_t)ESP.getMinFreeHeap();
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

    // GET /api/diag/gatt — GATT topology captured at the last connect
    // (services + chars + properties). Feeds the Compatibility Report so
    // users can map a new charger without nRF Connect.
    _async.on("/api/diag/gatt", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        String topo;
        wallboxBLE.copyGattTopology(topo);
        if (topo.isEmpty()) topo = "(no topology captured yet — connect to the charger first)\n";
        AsyncWebServerResponse* res = req->beginResponse(200, "text/plain", topo);
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
    // GET /api/config/export — JSON dump of current config with
    // secret fields masked. Audit fix: was missed during the
    // per-route migration and surfaced after the port swap.
    _async.on("/api/config/export", HTTP_GET,
              [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/json", ::wb_buildConfigExportJson());
        res->addHeader("Content-Disposition",
            "attachment; filename=\"wallbox-config.json\"");
        req->send(res);
    });

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

    // GET /style.css — shared dashboard stylesheet. The literal lives
    // in wb_web.cpp behind ::wb_getStyleCssLiteral(); same
    // Cache-Control header as the sync handler (immutable / 1 year
    // — URL carries ?v=<buildVer> as the cache-bust signal).
    _async.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res = req->beginResponse(200,
            "text/css", ::wb_getStyleCssLiteral());
        res->addHeader("Cache-Control",
            "public, max-age=31536000, immutable");
        req->send(res);
    });

    // GET /app.js — shared dashboard JS. Same caching contract as
    // /style.css; was a missed migration in step C that surfaced
    // only after step J flipped async onto port 80.
    _async.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res = req->beginResponse(200,
            "application/javascript", ::wb_getAppJsLiteral());
        res->addHeader("Cache-Control",
            "public, max-age=31536000, immutable");
        req->send(res);
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

// =====================================================================
// HTML pages — each dispatches to a wb_buildXxxPage() helper extracted
// from the sync handler in wb_web.cpp. Same builder, same output —
// the only thing the async server does differently is dispatch on
// the AsyncTCP task instead of the main loop.
// =====================================================================

static void _registerHtmlPages() {

    // GET / — STA-mode landing. Sync server only registers `/` for
    // the dashboard in STA mode (the `/dashboard` path is captive-
    // portal-only). Match that to keep sync vs async behavior
    // identical.
    _async.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        // v3.1 (#103): the dashboard is now a ~2 KB shell that fetches
        // /dashboard/body.gz from PROGMEM, so the old 36 KB heap guard is
        // gone — "/" no longer 503s under heap pressure.
        _sendHtmlPage(req, wb_buildDashboardPage());
    });

    // GET /dashboard/body.gz — pre-gzipped "/" dashboard body (v3.1 #103).
    // PROGMEM, no heap. "/" only exact-matches, so order vs "/" is moot, but
    // it lives with the other body.gz routes for clarity.
    _async.on("/dashboard/body.gz", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html",
            (const uint8_t*)DASH_BODY_GZ, DASH_BODY_GZ_LEN);
        res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        res->addHeader("X-Uncompressed-Bytes", String((unsigned long)DASH_BODY_RAW_LEN));
        req->send(res);
    });

    // GET /config — config UI. No auth gate on the page itself
    // (matches sync behavior — the config page is the entry point
    // for setting up auth in the first place).
    _async.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkHeapHeadroom(req, 24000)) return;
        _sendHtmlPage(req, wb_buildConfigPage());
    });

    // GET /setup — first-time onboarding wizard. Same page builder as
    // the sync server uses in AP mode; here in STA it's available for
    // users who want to re-run a guided setup (after a factory reset
    // dance, or to update creds without diving into /config's tabs).
    _async.on("/setup", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkHeapHeadroom(req, 30000)) return;
        _sendHtmlPage(req, wb_buildSetupPage());
    });

    // GET /info/body.gz — pre-gzipped /info body (v3.1 #103). PROGMEM, no
    // heap. MUST register BEFORE /info: AsyncWebServer prefix-matches a
    // path-bearing URI, so /info would otherwise swallow /info/body.gz.
    _async.on("/info/body.gz", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html",
            (const uint8_t*)INFO_BODY_GZ, INFO_BODY_GZ_LEN);
        res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        res->addHeader("X-Uncompressed-Bytes", String((unsigned long)INFO_BODY_RAW_LEN));
        req->send(res);
    });

    // GET /info — now just a tiny shell that fetches /info/body.gz above, so
    // the old 40 KB build + heap guard are gone (v3.1 #103). The body never
    // becomes a runtime String — it streams from PROGMEM.
    _async.on("/info", HTTP_GET, [](AsyncWebServerRequest* req) {
        wb_health::setBreadcrumbPath("/info");
        _sendHtmlPage(req, wb_buildInfoPage());
    });

    // GET /sessions/body.gz — pre-gzipped /sessions body (v3.1 #103). PROGMEM,
    // no heap. MUST register BEFORE /sessions: AsyncWebServer prefix-matches a
    // path-bearing URI, so /sessions would otherwise swallow /sessions/body.gz.
    _async.on("/sessions/body.gz", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html",
            (const uint8_t*)SESS_BODY_GZ, SESS_BODY_GZ_LEN);
        res->addHeader("Content-Encoding", "gzip");
        res->addHeader("Cache-Control", "no-store");
        res->addHeader("X-Uncompressed-Bytes", String((unsigned long)SESS_BODY_RAW_LEN));
        req->send(res);
    });

    // GET /sessions — charging sessions heatmap. Now a ~2 KB shell that
    // fetches /sessions/body.gz from PROGMEM (v3.1 #103), so the old 20 KB
    // heap guard is gone — /sessions no longer 503s under heap pressure.
    _async.on("/sessions", HTTP_GET, [](AsyncWebServerRequest* req) {
        _sendHtmlPage(req, wb_buildSessionsPage());
    });

    // GET /ota — firmware update page. Auth required.
    _async.on("/ota", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkHeapHeadroom(req, 20000)) return;
        _sendHtmlPage(req, wb_buildOtaPage());
    });

    // GET /logs — auto-refreshing log viewer. Auth required.
    _async.on("/logs", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkHeapHeadroom(req, 16000)) return;
        _sendHtmlPage(req, wb_buildLogsPage());
    });

    // GET /settings/body.gz — pre-gzipped PROGMEM blob (task #75).
    // The blob is sent verbatim with Content-Encoding: gzip; the
    // browser decompresses on its side. beginResponse_P streams
    // directly from flash without copying into RAM.
    //
    // IMPORTANT: must be registered BEFORE /settings. AsyncWebServer
    // returns the first matching handler, and its canHandle() does
    // a prefix match when the registered URI has a path. /settings
    // would otherwise greedily match /settings/body.gz too.
    _async.on("/settings/body.gz", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            if (!_checkAuth(req)) return;
            AsyncWebServerResponse* res = req->beginResponse_P(200,
                "text/html",
                (const uint8_t*)SETTINGS_BODY_GZ,
                SETTINGS_BODY_GZ_LEN);
            res->addHeader("Content-Encoding", "gzip");
            res->addHeader("Cache-Control", "public, max-age=300");
            res->addHeader("X-Uncompressed-Bytes",
                String((unsigned long)SETTINGS_BODY_RAW_LEN));
            req->send(res);
        });

    // GET /settings — shell page (small). The browser fetches the
    // ~56 KB body separately from /settings/body.gz so we don't
    // need chunked encoding here.
    _async.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        _sendHtmlPage(req, wb_buildSettingsPage());
    });
}

// =====================================================================
// Form-POST routes (/save, /reset). Field assignment + page-building
// extracted into wb_applySaveForm() / wb_applyResetAndPage() in
// wb_web.cpp; this just dispatches.
//
// NOTE: /api/config/import is NOT in this batch. It accepts a JSON
// body which AsyncWebServer needs to accumulate via a separate
// body-handler signature. Deferred to a follow-on commit so the
// pattern can be designed cleanly rather than retrofitted here.
// =====================================================================

static void _registerFormPostRoutes() {

    // POST /save — config form. wb_applySaveForm() reads ~20 fields
    // via the getter callable. Pass a lambda that reads from the
    // async request's form-encoded body (req->getParam(name, true)
    // — the `true` is `is_post`).
    _async.on("/save", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkCsrf(req)) return;
        String page = wb_applySaveForm([req](const char* k) -> String {
            if (req->hasParam(k, true)) {
                return req->getParam(k, true)->value();
            }
            return String("");
        });
        req->send(200, "text/html", page);
        webServer.requestReboot();
    });

    // POST /reset — factory reset + reboot.
    _async.on("/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!_checkCsrf(req)) return;
        req->send(200, "text/html", wb_applyResetAndPage());
        webServer.requestReboot();
    });
}

// =====================================================================
// BLE-passthrough routes — the architectural reason for this whole
// migration. /api/command in particular is the route that 2.7.0's
// chunked-wait pump (step 9c) was working around. Running on
// AsyncTCP gives us a separate-from-main-loop execution context;
// xTaskNotifyWait here doesn't block the main task, doesn't need the
// pump.
//
// Trade-off: blocking AsyncTCP for the BAPI wait queues OTHER async
// requests behind it. AsyncTCP processes requests serially. During
// the migration window this is fine — most clients hit sync on port
// 80; after retiring sync, we'll revisit (deferred-response via SSE
// or req->_tempObject + cross-task wake from BLE drain).
//
// /api/ble-scan and /api/wifi-scan are similar — they block AsyncTCP
// for ~5-8 s but that's the same time the sync server blocks on its
// own task. No regression.
// =====================================================================

static void _registerBleRoutes() {

    // GET /api/ble-scan — kicks off an 8 s BLE scan, returns devices
    // as JSON. Blocking call on AsyncTCP task — extend TWDT so the
    // 5 s default doesn't kill the task mid-scan (manifests in the
    // browser as "TypeError: Failed to fetch" because the connection
    // drops when AsyncTCP panics).
    _async.on("/api/ble-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        wb_wdt::extendTo(15);
        String body = wb_runBleScan();
        wb_wdt::restore();
        req->send(200, "application/json", body);
    });

    // GET /api/wifi-scan — kicks off a WiFi scan (~5 s).
    // Async server always runs in STA mode (started by wb_web_async::
    // begin() only when WiFi is up), so the AP-bypass branch from the
    // sync handler isn't applicable here — always auth-gate.
    // Same TWDT extension as ble-scan above — scanNetworks(false, ...,
    // 400 ms/channel) × 14 channels can hit ~5.6 s, right at the
    // default WDT threshold.
    _async.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        wb_wdt::extendTo(15);
        ScanResult r = wb_runWifiScan();
        wb_wdt::restore();
        req->send(r.status, "application/json", r.body);
    });

    // GET /api/command — the BAPI passthrough. Mirrors the sync
    // handler logic except:
    //   - No `?sync=1` escape hatch (deprecated, sync server still
    //     hosts it for legacy callers).
    //   - No chunked-pump (AsyncTCP task isn't shared with main loop,
    //     so blocking here doesn't starve MQTT/WS).
    //   - Single xTaskNotifyWait with the full waitMs timeout.
    _async.on("/api/command", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_checkAuth(req)) return;
        if (!wallboxBLE.isConnected()) {
            req->send(503, "application/json",
                "{\"error\":\"BLE not connected\"}");
            return;
        }
        if (!tbAllow()) {
            AsyncWebServerResponse* res = req->beginResponse(429,
                "application/json",
                "{\"error\":\"rate_limited\",\"retry\":true}");
            res->addHeader("Retry-After", "1");
            req->send(res);
            return;
        }
        // ?wait=N: 0..8000 default 5000. Same clamp as sync. A larger
        // cap (tried 16000) starves the BLE task long enough to trigger
        // a watchdog panic — keep the cap at 8s.
        int waitMs = 5000;
        if (req->hasParam("wait")) {
            waitMs = req->getParam("wait")->value().toInt();
            if (waitMs < 0) waitMs = 0;
            if (waitMs > 8000) waitMs = 8000;
        }

        // Resolve action -> met + par.
        String action = req->hasParam("action")
            ? req->getParam("action")->value() : String("");
        // Crash-trace breadcrumb: record the action so a panic in
        // command dispatch land tells us which one was in flight.
        {
            String trace = "/api/command?action=" + action;
            wb_health::setBreadcrumbPath(trace.c_str());
        }
        String value = req->hasParam("value")
            ? req->getParam("value")->value() : String("");
        const char* met = nullptr;
        String par;
        if      (action == "start")   { met = bapi::MET_START_STOP;  par = "1"; }
        // w_cha stop par: 2 for Pulsar MAX, 0 for Pulsar Plus family.
        // Plus ACKs par=2 but ignores it (charger keeps charging);
        // par=0 is the pause/stop value per jagheterfredrik/wallbox-ble.
        // Reported by peter-mcc on issue #4.
        else if (action == "stop")    { met = bapi::MET_START_STOP;  par = configMgr.isPlusFamily() ? "0" : "2"; }
        // Resume clears the schedule/eco manual-override flag
        // (r_dat.gen != 0 -> 0). The charger rejects s_cmode mode=0
        // (subcode 6) when actively charging (st=1), so we queue a
        // Stop first as a defensive prefix. Stop is a no-op when the
        // charger is already not charging — w_cha par=0/2 returns
        // r:null without changing state. Net effect: Resume always
        // lands gen=0 regardless of starting state. Two-call sequence
        // serializes through the BLE queue.
        else if (action == "resume")  {
            const char* stopPar = configMgr.isPlusFamily() ? "0" : "2";
            wallboxBLE.enqueueRequest(bapi::MET_START_STOP, stopPar);
            met = "s_cmode";             par = "{\"mode\":0}";
        }
        else if (action == "lock")    { met = bapi::MET_LOCK;        par = "1"; }
        else if (action == "unlock")  { met = bapi::MET_LOCK;        par = "0"; }
        else if (action == "current") { met = bapi::MET_SET_CURRENT; par = value; }
        else if (action == "reboot")  { met = bapi::MET_REBOOT;      par = "null"; }
        else if (action == "bapi") {
            // Use a per-request String for the met arg so its c_str()
            // outlives the lookup below. The static trick from sync
            // handler isn't safe across AsyncTCP requests.
            String userMet = req->hasParam("met")
                ? req->getParam("met")->value() : String("");
            if (userMet.length() == 0) {
                req->send(400, "application/json",
                    "{\"error\":\"missing met\"}");
                return;
            }
            // Stash on the request's tempObject for the lifetime of
            // the handler. We dispose at the bottom.
            String* metStorage = new String(userMet);
            met = metStorage->c_str();
            par = req->hasParam("par")
                ? req->getParam("par")->value() : String("null");
            if (par.isEmpty()) par = "null";
            // We'll delete this at end of handler — see below.
            req->onDisconnect([metStorage]() { delete metStorage; });
        } else {
            req->send(400, "application/json",
                "{\"error\":\"unknown action\"}");
            return;
        }

        // Async path. Enqueue with WAKE_AND_MQTT so a missed wake
        // still publishes to wallbox/response/<met> for polling.
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        xTaskNotifyStateClear(self);
        uint32_t bapiTimeout = (waitMs > 0)
            ? ((uint32_t)waitMs + 250) : 0;
        uint32_t reqId = wallboxBLE.enqueueRequest(
            met, par.c_str(),
            WallboxBLE::ReplyMode::WAKE_AND_MQTT, self, bapiTimeout);
        if (reqId == 0) {
            AsyncWebServerResponse* res = req->beginResponse(503,
                "application/json", "{\"error\":\"busy\",\"retry\":true}");
            res->addHeader("Retry-After", "1");
            req->send(res);
            return;
        }

        if (waitMs == 0) {
            // Pure async — return 202 + id immediately.
            String body = "{\"id\":" + String(reqId)
                        + ",\"status\":\"pending\"}";
            req->send(202, "application/json", body);
            return;
        }

        // Block AsyncTCP task on the per-request notify. Sliced to
        // 2-second chunks so we can feed the TWDT between slices.
        // The AsyncTCP task is subscribed to the task watchdog (5s
        // default) — a single waitMs up to 8000ms would trip it.
        // esp_task_wdt_reset() is a no-op if this task isn't
        // subscribed, so the call is safe either way.
        uint32_t notified = 0;
        BaseType_t got = pdFALSE;
        uint32_t remaining = (uint32_t)waitMs;
        while (remaining > 0) {
            uint32_t slice = remaining > 2000 ? 2000 : remaining;
            got = xTaskNotifyWait(0, ULONG_MAX, &notified,
                                  pdMS_TO_TICKS(slice));
            if (got == pdTRUE) break;
            remaining -= slice;
            esp_task_wdt_reset();
        }
        if (got == pdTRUE) {
            String resp;
            if (wallboxBLE.tryFetchResponse(reqId, resp) && resp.length()) {
                req->send(200, "application/json", resp);
                return;
            }
        }
        // Deadline elapsed or notify race lost. Same fallback as sync:
        // BLE task will still complete and publish to MQTT; the caller
        // can poll /api/command_status?id=N.
        String body = "{\"id\":" + String(reqId) + ",\"status\":\"pending\"}";
        req->send(202, "application/json", body);
    });
}

// 3.0 task #78 step G — routes that take a JSON request body.
// AsyncWebServer doesn't surface POST bodies via getParam(); they
// arrive as chunked callbacks (the 5-arg "body handler" overload).
// We accumulate chunks into a request-scoped malloc'd buffer parked
// on req->_tempObject (which AsyncWebServerRequest's destructor
// free()s automatically), then the request handler runs once after
// the body is fully received and dispatches to wb_applyConfigImport.
static void _registerJsonBodyRoutes() {
    // Hard cap on accepted body size. Live config JSON is ~1-2 KB;
    // 8 KB is generous and small enough to fit comfortably in heap
    // alongside the other request state.
    constexpr size_t MAX_IMPORT_BODY = 8 * 1024;

    _async.on("/api/config/import", HTTP_POST,
        // requestHandler — called AFTER body is fully received.
        [](AsyncWebServerRequest* req) {
            if (!_checkAuth(req)) return;
            if (!_checkCsrf(req)) return;
            const char* buf = (const char*)req->_tempObject;
            if (!buf) {
                req->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"missing body\"}");
                return;
            }
            ImportResult r = wb_applyConfigImport(String(buf));
            req->send(r.status, "application/json", r.body);
            if (r.reboot) webServer.requestReboot();
        },
        // uploadHandler — not used; this endpoint is JSON not multipart.
        NULL,
        // bodyHandler — invoked one or more times with body chunks.
        // Allocate the accumulation buffer on the FIRST chunk (when
        // `total` is known and `index` == 0). Reject oversize bodies
        // up front so we don't burn heap accumulating then bail.
        [MAX_IMPORT_BODY](AsyncWebServerRequest* req, uint8_t* data,
                          size_t len, size_t index, size_t total) {
            if (index == 0) {
                if (total == 0 || total > MAX_IMPORT_BODY) {
                    req->_tempObject = nullptr;
                    return;
                }
                char* buf = (char*)malloc(total + 1);
                if (!buf) {
                    req->_tempObject = nullptr;
                    return;
                }
                buf[total] = '\0';
                req->_tempObject = buf;
            }
            char* buf = (char*)req->_tempObject;
            if (buf && (index + len) <= MAX_IMPORT_BODY) {
                memcpy(buf + index, data, len);
            }
        }
    );
}

// =====================================================================
// 3.0 task #78 step I — /api/ota (firmware upload).
//
// Distinct route because AsyncWebServer has a dedicated upload-handler
// signature for multipart/form-data:
//   onUpload(req, filename, index, data, len, final)
// The per-chunk callback runs on AsyncTCP. The request handler (the
// first lambda) runs ONCE AFTER all chunks have been consumed — that
// is where we MUST send the response (sending from inside the upload
// handler is rejected by the library).
//
// We deliberately mirror the sync handler's behaviour byte-for-byte:
//   - auth check at the very first chunk before Update.begin() erases
//   - admission guard (otaInProgress + wb_health::canAcceptOta)
//   - BLE paused for the OTA window (5 min) — radio coex starves WiFi
//   - WDT extended to 60 s — Update.begin() erases the partition
//     synchronously and can take >5 s on a fresh partition
//   - first-byte magic check (ESP32 image starts with 0xE9)
//   - optional X-Firmware-MD5 integrity check
//   - truncation tolerance ±256 bytes for the multipart envelope
//   - response classification: 503+Retry-After for admission rejects,
//     500 for hard failures, 200 + reboot for success
//
// The reboot does NOT call ESP.restart() from the request handler.
// Instead we set webServer.requestReboot() — the main loop will
// observe the flag and ESP.restart() ~2 s later (see
// WBWebServer::loop). That gives AsyncTCP time to flush the 200
// response back to the client before the device dies.
//
// SYNC HANDLER STAYS IN PLACE. While the async route is fresh, the
// sync server still serves /api/ota on port 80; a broken async path
// can't brick the gateway because the user can fall back. Once the
// async OTA is exercised across both botts7+peter-mcc test rigs,
// the port swap can land and the sync handler retires.
// =====================================================================

// File-static OTA progress state. Safe to share across requests
// because the otaInProgress guard ensures only one upload is ever
// in flight. Declared at file scope so both lambdas inside
// _registerOtaRoute can capture them.
static bool   _asyncOtaError        = false;
static bool   _asyncOtaShouldReboot = false;
static size_t _asyncOtaTotalSize    = 0;
// Set by the body handler when a non-multipart (raw) body hits /api/ota.
// A real OTA is multipart/form-data and is dispatched to the upload
// handler; anything reaching the body handler is malformed.
static bool   _asyncOtaRawRejected  = false;

static void _registerOtaRoute() {
    _async.on("/api/ota", HTTP_POST,
        // requestHandler — runs ONCE after the upload completes
        // (or fails). Examines the file-static result vars below and
        // sends the appropriate response.
        [](AsyncWebServerRequest* req) {
            // Raw/malformed POST (no multipart part): the upload handler
            // never ran, so without this the empty-upload path would send
            // a misleading 200 "OK". Return 415 instead. (Reset the flag
            // for the next request.)
            if (_asyncOtaRawRejected) {
                _asyncOtaRawRejected = false;
                req->send(415, "application/json",
                    "{\"error\":\"OTA requires multipart/form-data with a "
                    "'firmware' file part; raw request bodies are rejected\"}");
                return;
            }
            if (_asyncOtaError) {
                if (otaRetryAfterSec > 0) {
                    AsyncWebServerResponse* res = req->beginResponse(503,
                        "application/json",
                        String("{\"error\":\"") + otaRejectReason
                            + "\",\"retry_after\":"
                            + String(otaRetryAfterSec) + "}");
                    res->addHeader("Retry-After",
                        String(otaRetryAfterSec));
                    req->send(res);
                } else {
                    req->send(500, "text/plain", "Upload failed");
                }
            } else {
                req->send(200, "text/plain", "OK");
                if (_asyncOtaShouldReboot) {
                    // Defer the actual ESP.restart() to the main loop —
                    // see WBWebServer::loop. ~2 s after the flag is set
                    // the loop calls ESP.restart(), giving AsyncTCP
                    // time to flush this 200.
                    webServer.requestReboot();
                }
            }
        },
        // uploadHandler — per-chunk. State (totalSize, error) lives
        // file-static below; the otaInProgress guard ensures only one
        // OTA is ever in flight so static is safe.
        [](AsyncWebServerRequest* req, String filename,
           size_t index, uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                // ---- START ----
                otaRetryAfterSec = 0;
                otaRejectReason  = String();
                _asyncOtaError = false;
                _asyncOtaShouldReboot = false;
                _asyncOtaTotalSize = 0;

                // SECURITY: auth check BEFORE Update.begin() erases
                // the partition. _checkAuth handles the 401 response;
                // we just need to flag the error so the request
                // handler stays quiet (AsyncWebServer requires the
                // request handler to send something or the connection
                // hangs; we send 500 in the error path below).
                if (!_checkAuth(req)) {
                    _asyncOtaError = true;
                    return;
                }
                if (otaInProgress) {
                    _asyncOtaError    = true;
                    otaRetryAfterSec  = 10;
                    otaRejectReason   = "another OTA already in progress";
                    return;
                }
                String reason;
                if (!wb_health::canAcceptOta(reason)) {
                    _asyncOtaError    = true;
                    otaRetryAfterSec  =
                        (uint16_t)wb_health::otaRetryAfterSeconds();
                    otaRejectReason   = reason;
                    return;
                }

                Log.printf("[OTA-async] Upload start: %s\n",
                    filename.c_str());
                otaInProgress = true;

                wallboxBLE.pause(5 * 60 * 1000);  // 5 min

                size_t expected = (size_t)req->contentLength();
                expectedOtaSize = expected;

                const esp_partition_t* partition =
                    esp_ota_get_next_update_partition(NULL);
                // Size sanity-check BEFORE extending the WDT so an early
                // return here doesn't leak the relaxed timeout (task
                // #106 audit fix).
                if (partition && expected > 0 &&
                        expected > partition->size) {
                    Log.printf("[OTA-async] REJECTED: payload (%u) "
                               "larger than partition (%u)\n",
                               (unsigned)expected,
                               (unsigned)partition->size);
                    _asyncOtaError = true;
                    otaInProgress  = false;
                    return;
                }

                wb_wdt::extendTo(60);  // erase can take >5 s

                bool ok = expected > 0
                    ? Update.begin(expected)
                    : Update.begin(UPDATE_SIZE_UNKNOWN);
                if (!ok) {
                    Log.printf("[OTA-async] Begin failed: %s\n",
                        Update.errorString());
                    // Restore before returning — `final` may not fire to
                    // clean up later. Idempotent if it does.
                    wb_wdt::restore();
                    _asyncOtaError = true;
                    return;
                }
                if (req->hasHeader("X-Firmware-MD5")) {
                    String md5 =
                        req->getHeader("X-Firmware-MD5")->value();
                    md5.trim();
                    md5.toLowerCase();
                    if (md5.length() == 32) {
                        if (Update.setMD5(md5.c_str())) {
                            Log.printf("[OTA-async] Expecting MD5 %s\n",
                                md5.c_str());
                        }
                    }
                }
            }

            if (!_asyncOtaError && len > 0) {
                // ---- WRITE ----
                if (_asyncOtaTotalSize == 0 && len > 0) {
                    if (data[0] != 0xE9) {
                        Log.println("[OTA-async] REJECTED: not ESP32 "
                                    "firmware (magic byte != 0xE9)");
                        Update.abort();
                        _asyncOtaError = true;
                        return;
                    }
                    Log.println("[OTA-async] Firmware magic byte OK");
                }
                if (Update.write(data, len) != len) {
                    Log.printf("[OTA-async] Write failed: %s\n",
                        Update.errorString());
                    _asyncOtaError = true;
                }
                _asyncOtaTotalSize += len;
            }

            if (final) {
                // ---- END ----
                otaInProgress = false;
                wb_wdt::restore();

                if (!_asyncOtaError && expectedOtaSize > 0) {
                    size_t diff = (_asyncOtaTotalSize < expectedOtaSize)
                        ? expectedOtaSize - _asyncOtaTotalSize
                        : _asyncOtaTotalSize - expectedOtaSize;
                    if (diff > 256) {
                        Log.printf("[OTA-async] TRUNCATED: expected ~%u "
                                   "bytes, got %u — aborting\n",
                                   (unsigned)expectedOtaSize,
                                   (unsigned)_asyncOtaTotalSize);
                        _asyncOtaError = true;
                    }
                }

                if (_asyncOtaError) {
                    Update.abort();
                    if (otaRetryAfterSec > 0) {
                        wb_ota_history::recordOta(millis() / 1000,
                            WB_VERSION, _asyncOtaTotalSize, false,
                            (String("rejected: ") + otaRejectReason).c_str());
                    } else {
                        wb_ota_history::recordOta(millis() / 1000,
                            WB_VERSION, _asyncOtaTotalSize, false,
                            "aborted");
                    }
                } else if (Update.end(true)) {
                    Log.printf("[OTA-async] Success! %u bytes written\n",
                        (unsigned)_asyncOtaTotalSize);
                    wb_ota_history::recordOta(millis() / 1000,
                        WB_VERSION, _asyncOtaTotalSize, true, "ok");
                    wb_health::markOtaSuccess();
                    _asyncOtaShouldReboot = true;
                } else {
                    Log.printf("[OTA-async] End failed: %s\n",
                        Update.errorString());
                    wb_ota_history::recordOta(millis() / 1000,
                        WB_VERSION, _asyncOtaTotalSize, false,
                        Update.errorString());
                    _asyncOtaError = true;
                }
            }
        },
        // bodyHandler — fires ONLY for non-multipart request bodies. A
        // multipart upload is dispatched to the upload handler above and
        // never reaches here (see ESPAsyncWebServer WebRequest.cpp: a
        // multipart body goes through _parseMultipartPostByte, a raw body
        // through handleBody). So any body landing here is a raw/malformed
        // POST. Flag it on the first chunk; the request handler turns that
        // into a 415. The bytes still stream through harmlessly (they're
        // not buffered), but the firmware Update path never touches them.
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            (void)req; (void)data; (void)len; (void)total;
            if (index == 0) _asyncOtaRawRejected = true;
        }
    );
}

void begin() {
    _registerReadOnlyRoutes();
    _registerStaticAndPostRoutes();
    _registerHtmlPages();
    _registerFormPostRoutes();
    _registerBleRoutes();
    _registerJsonBodyRoutes();
    _registerOtaRoute();
    // 3.0 task #82: register the AsyncWebSocket handler so /ws lives
    // on the same port (:80) as everything else. wbws::begin() is
    // called from main.cpp's setup() to wire the onEvent callback;
    // here we just plumb the handler into the async webserver before
    // starting it.
    _async.addHandler(&wbws::handler());
    // ESPAsyncWebServer doesn't auto-respond 404 for unregistered
    // routes — without onNotFound() it falls through to a 500. Match
    // the sync server's behavior (the sync WebServer auto-404s).
    _async.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not Found");
    });
    _async.begin();
    Log.println("[Web-async] Listening on :80 (production, post port-swap)");
}

}  // namespace wb_web_async

#else  // !WB_ASYNC_WEB

// Flag OFF: stub. begin() compiles to a no-op return; the async
// library symbols are unreferenced and dead-stripped by the linker.
namespace wb_web_async {
void begin() {}
}

#endif  // WB_ASYNC_WEB
