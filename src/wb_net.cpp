#include "wb_net.h"
#include "wb_config.h"
#include "wb_log.h"
#include "wb_diag.h"
#include "wb_version.h"

#include <WiFi.h>
#include <ESPmDNS.h>

namespace wb_net {

// Rebuild mDNS responder + service records. Used both at initial
// boot (after the blocking connect succeeds) and on every GOT_IP
// after a reconnect — the mDNS bind is IP-scoped, so a new IP
// invalidates the previous responder. addService() calls must
// re-run too or service-discovery clients won't see http/telnet.
//
// Safe to call from main task only — uses LWIP API.
static void _setupMdns() {
    MDNS.end();
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = MDNS.begin("wallbox-gw");
        if (!ok) delay(500);
    }
    if (!ok) {
        Log.println("[mDNS] failed to start — use IP address directly");
        return;
    }
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "device", "wallbox-gateway");
    MDNS.addServiceTxt("http", "tcp", "version", WB_VERSION);
    MDNS.addServiceTxt("http", "tcp", "path", "/");
    MDNS.addService("telnet", "tcp", 23);
    Log.printf("[mDNS] http://wallbox-gw.local (IP: %s)\n",
               WiFi.localIP().toString().c_str());
}

// ---- Cached state, written by WiFi event task, read by main task ----
//
// All single-word volatile types — atomic on Xtensa LX7. Event handlers
// only touch these; mDNS refresh and the actual WiFi.reconnect() call
// are deferred to tick() on main.
static volatile bool     _connected            = false;
static volatile uint32_t _lastConnectedAt      = 0;
static volatile uint32_t _lastDisconnectedAt   = 0;
static volatile bool     _pendingMdnsRefresh   = false;  // GOT_IP fired → tick() rebinds mDNS
static volatile bool     _pendingReportRecon   = false;  // GOT_IP fired → tick() calls wb_diag::reportReconnect(WIFI)
static volatile bool     _pendingReportDiscon  = false;  // STA_DISCONNECTED → tick() calls wb_diag::reportDisconnect(WIFI)

// ---- Backoff state, written + read by main task only ----
//
// _backoffMs walks the 1/2/5/15/60 s ladder. _nextAttemptAtMs is the
// absolute millis() deadline; 0 = no pending explicit reconnect.
// Driver's own auto-reconnect runs continuously in the background —
// we only call WiFi.reconnect() explicitly if the driver has clearly
// given up (≥ 60 s since last GOT_IP).
static uint32_t _backoffMs           = 0;
static uint32_t _nextAttemptAtMs     = 0;
static const uint32_t kDriverGiveUpMs = 60000;

// Walk the backoff ladder: 1s → 2s → 5s → 15s → 60s → stays at 60s.
static uint32_t _nextBackoff(uint32_t cur) {
    if (cur < 1000)  return 1000;
    if (cur < 2000)  return 2000;
    if (cur < 5000)  return 5000;
    if (cur < 15000) return 15000;
    return 60000;
}

// ---- WiFi event handler — runs on driver's event task ----
//
// CRITICAL: only set flags / timestamps. No mDNS, no diag, no
// WiFi.reconnect(). The event-task stack is small and the framework
// reuses it across all events — heavy work here can stack-overflow or
// deadlock the driver. All real work runs in tick() on main.
//
// FORENSIC (task #97): also log STA_DISCONNECTED reason codes so the
// boot trace tells us whether a 20 s blocking begin() gave up because
// the SSID wasn't found, the password was wrong, the handshake timed
// out, or the AP actively dropped us.
static volatile uint8_t _lastDiscReason = 0;

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    uint32_t now = millis();
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
            if (!_connected) {
                _connected           = true;
                _lastConnectedAt     = now;
                _pendingMdnsRefresh  = true;
                _pendingReportRecon  = true;
            }
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            _lastDiscReason = info.wifi_sta_disconnected.reason;
            // fall through
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            if (_connected) {
                _connected           = false;
                _lastDisconnectedAt  = now;
                // Only log to diag if we'd been connected long enough
                // for this to be a real outage. Transient disconnects
                // during the framework's initial association would
                // otherwise be reported as a "WiFi reconnect" once
                // GOT_IP eventually arrives.
                if (_lastConnectedAt != 0 &&
                    (uint32_t)(now - _lastConnectedAt) > 5000) {
                    _pendingReportDiscon = true;
                }
            }
            break;
        default:
            break;
    }
}

uint8_t lastDisconnectReason() { return _lastDiscReason; }

bool begin() {
    const WBConfig& cfg = configMgr.get();
    if (cfg.wifiSSID.length() == 0) {
        Log.println("[WiFi] begin() early-return: wifiSSID empty");
        return false;
    }

    // Register event handler BEFORE WiFi.begin() so we catch the
    // initial GOT_IP. The framework dispatches events to all
    // registered handlers, so coexisting with any later registrants
    // is safe.
    WiFi.onEvent(onWiFiEvent);

    // FORENSIC (task #97): log SSID + pass length right before the
    // begin call so a boot trace shows exactly what was attempted.
    // Earlier ConfigManager::load() log shows what came out of NVS;
    // this shows what WiFi.begin was actually invoked with.
    Log.printf("[WiFi] Connecting to '%s' (ssid_len=%u, pass_len=%u)",
        cfg.wifiSSID.c_str(),
        (unsigned)cfg.wifiSSID.length(),
        (unsigned)cfg.wifiPass.length());
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSSID.c_str(), cfg.wifiPass.c_str());

    // Initial blocking connect — keeps the existing setup() contract.
    // tries × 500 ms = 20 s; matches the legacy connectWiFi() bound.
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        Log.print(".");
        tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Log.printf("\n[WiFi] Connected: %s\n",
                   WiFi.localIP().toString().c_str());
        // The event-task handler may or may not have fired before this
        // point depending on framework timing. Mirror state explicitly
        // so tick() sees a consistent picture.
        _connected          = true;
        _lastConnectedAt    = millis();
        _pendingMdnsRefresh = true;
        return true;
    }

    // FORENSIC (task #97): expose final WiFi.status() AND the latest
    // disconnect-reason code from the WiFi event task. Combined they
    // pinpoint the failure mode:
    //   status=1 / reason=201 NO_AP_FOUND  → SSID typo or 5 GHz-only
    //   status=4 / reason=202 AUTH_FAIL    → password wrong
    //   status=6 / reason=204 HANDSHAKE_TIMEOUT → password right but
    //                                            AP rejected anyway
    //   status=6 / reason=15 4WAY_HANDSHAKE_TIMEOUT → password wrong
    //                                                 (newer ESP-IDF)
    //   status=6 / reason=2 AUTH_EXPIRE  → password wrong, AP dropped
    Log.printf("\n[WiFi] Failed to connect (status=%d reason=%u after %d tries)\n",
        (int)WiFi.status(), (unsigned)_lastDiscReason, tries);
    return false;
}

bool isConnected() {
    return _connected;
}

void tick() {
    uint32_t now = millis();

    // ---- Drain pending event-driven work ----
    if (_pendingReportDiscon) {
        _pendingReportDiscon = false;
        wb_diag::reportDisconnect(wb_diag::Kind::WIFI);
        Log.println("[WiFi] driver reports disconnected — driver auto-reconnect runs in the background");
    }
    if (_pendingReportRecon) {
        _pendingReportRecon = false;
        wb_diag::reportReconnect(wb_diag::Kind::WIFI);
        // GOT_IP means whatever path got us back — driver auto-reconnect
        // OR our explicit reconnect — succeeded. Reset backoff so the
        // next disconnect starts fresh from 1 s.
        _backoffMs       = 0;
        _nextAttemptAtMs = 0;
    }
    if (_pendingMdnsRefresh) {
        _pendingMdnsRefresh = false;
        // mDNS responder is bound to the IP; on a new GOT_IP the bind
        // is stale. Tear down + rebuild with the service records too,
        // otherwise post-reconnect discovery clients see the host but
        // not the http/telnet services.
        _setupMdns();
    }

    // ---- Explicit reconnect gate ----
    //
    // The Arduino-ESP32 driver has its own auto-reconnect — on a
    // STA_DISCONNECTED it keeps trying transparently. We only step
    // in when:
    //   (a) we ARE disconnected right now, AND
    //   (b) the driver has had ≥ 60 s since last GOT_IP (it has
    //       clearly given up), AND
    //   (c) the backoff deadline has elapsed
    if (_connected) return;
    if (_lastConnectedAt == 0) return;  // never connected this boot — let begin()/AP-mode handle it
    if ((uint32_t)(now - _lastConnectedAt) < kDriverGiveUpMs) return;
    if (_nextAttemptAtMs != 0 && (int32_t)(now - _nextAttemptAtMs) < 0) return;

    // The driver has given up and our backoff window has elapsed.
    // Fire one explicit reconnect attempt. This CAN still block for
    // several seconds — but at most once per backoff window, not
    // every 30 s as in the pre-refactor checkWiFi().
    Log.printf("[WiFi] driver auto-reconnect appears stuck — calling WiFi.reconnect() (backoff was %u ms)\n",
               (unsigned)_backoffMs);
    WiFi.reconnect();
    _backoffMs       = _nextBackoff(_backoffMs);
    _nextAttemptAtMs = now + _backoffMs;
}

uint32_t lastConnectedAtMs()    { return _lastConnectedAt; }
uint32_t lastDisconnectedAtMs() { return _lastDisconnectedAt; }

void forceReconnect() {
    // Skip the 60 s "driver gave up" gate; still respect backoff.
    if (_nextAttemptAtMs != 0 && (int32_t)(millis() - _nextAttemptAtMs) < 0) {
        Log.println("[WiFi] forceReconnect() throttled by backoff");
        return;
    }
    Log.println("[WiFi] forceReconnect() — explicit caller");
    WiFi.reconnect();
    _backoffMs       = _nextBackoff(_backoffMs);
    _nextAttemptAtMs = millis() + _backoffMs;
}

}  // namespace wb_net
