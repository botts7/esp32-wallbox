#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "bapi.h"

// Wallbox BLE connection manager.
// Handles connect/reconnect, BAPI framing, PIN auth, notifications.
// Includes WiFi coexistence, keepalive, and smart reconnect.

class WallboxBLE {
public:
    enum class State {
        DISCONNECTED,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        ERROR
    };

    void begin(const char* addr);
    void loop();

    // Send a BAPI command (blocking, up to timeoutMs)
    String sendCommand(const char* met, const char* par = "null", uint32_t timeoutMs = 5000);

    // Non-blocking: queue a command
    void queueCommand(const char* met, const char* par = "null");

    // 2.7.0 async request queue (in-progress, see docs/plans/
    // 2.7.0-api-command-async.md). Public-section ReplyMode + BleReq
    // because `enqueueRequest` references them in default args.
    enum class ReplyMode : uint8_t {
        FIRE_AND_FORGET = 0,   // discard response
        WAKE_WAITER     = 1,   // xTaskNotify the originating task
        MQTT_PUBLISH    = 2,   // queue response for main task to publish
        WAKE_AND_MQTT   = 3,   // both — used by /api/command short-wait
                               // so a handler that times out (202) still
                               // has its response delivered to the
                               // wallbox/response/<met> topic for the
                               // client to pick up out-of-band
    };
    // Enqueues a BAPI request on the BLE task's internal queue and
    // returns immediately with the assigned request id (0 = queue
    // full or not yet initialised). The BLE task drains at ~50 Hz
    // and runs each request through sendCommand internally; the
    // response is stored in the RAM map (see tryFetchResponse).
    uint32_t enqueueRequest(const char* met,
                            const char* par = "null",
                            ReplyMode replyMode = ReplyMode::FIRE_AND_FORGET,
                            TaskHandle_t waiter = nullptr,
                            uint32_t bapiTimeoutMs = 0);

    // 2.7.0 step 3 — fetch a previously-enqueued request's response
    // by its assigned id. Returns true and fills `out` if the
    // response is available; false if the request hasn't completed
    // yet, or its entry has been FIFO-evicted from the small map
    // (cap kResponseMapSize). Callers race the map: poll, sleep,
    // poll again — or use xTaskNotifyWait (step 4) for the active
    // wake path.
    bool tryFetchResponse(uint32_t reqId, String& out);
    static const uint8_t kResponseMapSize = 4;

    // 2.7.0 step 5 — drain pending MQTT-publish responses queued by
    // the BLE-task drain when a request's replyMode was MQTT_PUBLISH.
    // Returns true and fills out_met + out_json if a publish is
    // pending; false if the queue is empty. Main task calls this in
    // its existing publishCached*IfNew block and publishes via
    // PubSubClient (which is not thread-safe — must stay on main
    // task). Same defer-via-queue pattern as _storeCache.
    bool drainPendingResponsePub(String& out_met, String& out_json);
    static const uint8_t kPendingPubSize = 4;

    // Step 9 hardening: snapshot of the next-to-be-issued request id.
    // /api/command_status uses this to distinguish "future id, never
    // issued" (410 Gone) from "could plausibly still be in flight" (202
    // pending) for an id that's already left the small response map.
    // Atomic load to match the atomic_fetch_add in enqueueRequest.
    uint32_t peekNextReqId() const {
        return __atomic_load_n(&_nextReqId, __ATOMIC_RELAXED);
    }

    // State
    State state() const { return _state; }
    bool isConnected() const { return _state == State::CONNECTED; }
    const char* stateStr() const;

    // Temporarily release BLE so the official app can connect
    void pause(uint32_t ms);
    bool isPaused() const { return _pausedUntil > millis(); }
    uint32_t pauseRemainingMs() const { return _pausedUntil > millis() ? _pausedUntil - millis() : 0; }

    // PIN
    void setPin(const char* pin) { _pin = pin; }
    bool pinRequired() const { return _pinRequired; }
    uint32_t blePasskey() const { return _pin.isEmpty() ? 0 : (uint32_t)_pin.toInt(); }

    // UUID override (single-char mode: write+notify on the same characteristic — Pulsar MAX)
    void setUUIDs(const char* svc, const char* chr) { _svcUUID = svc; _chrUUID = chr; _txChrUUID = ""; }
    // Dual-char mode (Pulsar Plus etc.): write on chr, notify on txChr.
    void setUUIDs(const char* svc, const char* chr, const char* txChr) {
        _svcUUID = svc; _chrUUID = chr; _txChrUUID = (txChr && *txChr) ? txChr : "";
    }

    // Charger model — drives auth model + keepalive choice.
    // Plus-family ("plus", "copper", "quasar", "quasar2") all share the
    // Nordic-UART-style dual-char protocol per jagheterfredrik/wallbox-ble
    // and -mqtt-bridge: no BAPI PIN, `r_dat` keepalive, Plus 0-18 status enum.
    void setChargerModel(const char* m) { _chargerModel = (m && *m) ? m : "max"; }
    bool isPlus() const {
        return _chargerModel == "plus" || _chargerModel == "copper"
            || _chargerModel == "quasar" || _chargerModel == "quasar2";
    }

    // Responses
    String lastResponse() const { return _lastResponse; }
    using ResponseCallback = void (*)(const String& json);
    void onResponse(ResponseCallback cb) { _responseCb = cb; }

    // Yield callback — called during BLE wait so web server stays responsive
    using YieldCallback = void (*)();
    void onYield(YieldCallback cb) { _yieldCb = cb; }

    // Phase-2 polling — cadence setters (defaults match rc15 config).
    // Main task calls these from setup() with the user's config values.
    void setStatusPollMs(uint32_t ms)   { if (ms >= 1000) _statusPollMs = ms; }
    void setRealtimePollMs(uint32_t ms) { if (ms >= 1000) _realtimePollMs = ms; }

    // Nudge the BLE task to run _pollRealtime + _pollSettings ahead of
    // its regular cadence. `delayMs` is the minimum wait before the
    // re-poll fires — the BAPI write returns as soon as the charger
    // acknowledges receipt, but the charger's internal state machine
    // can take another 100-500 ms to actually apply the change. A
    // re-poll fired immediately can hit that window, read the OLD
    // value, and undo an optimistic publish (the user-visible
    // "sometimes it bounces, sometimes it doesn't" symptom). 500 ms
    // is a safe default — long enough to clear typical apply latency
    // without making the UI feel laggy.
    void requestSettingsRepoll(uint32_t delayMs = 500) {
        // Position _lastRealtimePoll so the BLE task's
        //   now - _lastRealtimePoll >= _realtimePollMs
        // becomes true exactly `delayMs` from now.
        uint32_t now = millis();
        uint32_t target = (now + delayMs > _realtimePollMs)
                            ? (now + delayMs - _realtimePollMs) : 0;
        _lastRealtimePoll = target;
    }

    // Phase-2 polling — thread-safe copy-out accessors. Each one
    // copies the latest cached value plus its seq counter; main task
    // compares to its remembered "last published" seq and only
    // re-publishes when seq has advanced.
    void copyCachedStatus(String& out, uint32_t& seq);
    void copyCachedRealtime(String& out, uint32_t& seq);
    void copyCachedMeter(String& out, uint32_t& seq);
    void copyCachedSettings(String& out, uint32_t& seq);
    void copyCachedNotifications(String& out, uint32_t& seq);

    // Stats
    uint32_t txCount() const { return _txCount; }
    uint32_t rxCount() const { return _rxCount; }
    int rssi() const;
    int scanRSSI() const { return _scanRSSI; }
    uint32_t lastActivityAge() const { return millis() - _lastActivityTime; }

    // Device info (from GATT 0x180A Device Information service)
    String deviceManufacturer() const { return _devMfg; }
    String deviceModel() const { return _devModel; }
    String deviceFirmware() const { return _devFw; }
    String deviceName() const { return _devName; }

    // Charger-reported identity (read post-connect via BAPI r_sn_ / g_mac)
    String chargerSerial() const { return _chgSerial; }
    String chargerMac() const { return _chgMac; }
    // Grounding status (r_wel) — universal safety diagnostic.
    // Empty if not yet polled / not supported on this firmware.
    String chargerGrounding() const { return _chgGrounding; }

    // Charger application firmware (fw_v_.s) — the version Wallbox app
    // shows the user (e.g. "6.11.16" on MAX, "6.7.38" on Plus). This is
    // distinct from deviceFirmware() which is the BLE module's firmware.
    String chargerAppFirmware() const { return _chgAppFw; }

    // True iff fw_v_ has been read and HA discovery should be
    // re-published so the device's sw_version reflects the real charger
    // firmware instead of the WB_VERSION fallback. Cleared by the main
    // loop after it triggers the re-publish.
    bool discoveryStale() const { return _discoveryStale; }
    void clearDiscoveryStale() { _discoveryStale = false; }
    // Charger project name (fw_v_.p, e.g. "prj15-pulsar-max"). Source of
    // truth for charger model — preferred over the user-config dropdown.
    String chargerProject() const { return _chgProject; }
    // Total number of charging sessions recorded on the charger (r_ses.size).
    // Read at BLE init + refreshed on the slow settings-poll cadence.
    // -1 = not yet read / unsupported.
    int32_t chargerSessionCount() const { return _chgSessionCount; }

    // Power Boost current limit (r_hsh) — the household-meter-tied
    // dynamic current cap. Returns the configured value as an integer
    // (amps on observed chargers). -1 = not yet read / unsupported.
    int32_t chargerPowerBoost() const { return _chgPowerBoost; }

    // Discrete lock status (r_lck) — 0 = unlocked, 1 = locked, -1 = unread.
    // Same data as r_sta.lock_status but exposed as a dedicated read
    // for cleaner HA lock-entity wiring.
    int32_t chargerLockState() const { return _chgLockState; }

    // Charger-side network status (gnsta) — IP/gateway/DNS/SSID/RSSI
    // as the charger sees them. Distinct from the gateway's own WiFi
    // (the ESP32 connects to user's WiFi; the charger connects to its
    // OWN WiFi for cloud / OCPP / firmware updates). Returns the
    // first network entry as a stringified summary.
    String chargerNetworkSsid() const { return _chgNetSsid; }
    String chargerNetworkIp()   const { return _chgNetIp; }
    // gnsta returns a 0-100 quality percentage in the `signal` field
    // (no `rssi`/dBm field on Pulsar MAX). Surfaced as a percent.
    int    chargerNetworkSignal() const { return _chgNetSignal; }

    // Best-effort mapping from fw_v_.p to our internal model key
    // (max/plus/copper/quasar/quasar2). Empty when fw_v_ hasn't been
    // read yet or the project string doesn't match a known prefix.
    // Read-only — does NOT update cfg.chargerModel (a wrong user
    // config would have prevented BLE from connecting in the first
    // place, so a mismatch at this point is informational, not load-
    // bearing). Used by /info to surface "your config vs what the
    // charger says" for the user to manually reconcile.
    String inferredModel() const {
        if (_chgProject.length() == 0) return "";
        // Lowercase strstr-style match — projects observed so far:
        //   "prj15-pulsar-max" (MAX, confirmed)
        //   pulsar-plus / copper / quasar / quasar2 expected to follow
        //   the same pattern based on jagheterfredrik's project list.
        if (_chgProject.indexOf("pulsar-max") >= 0) return "max";
        if (_chgProject.indexOf("pulsar-plus") >= 0) return "plus";
        if (_chgProject.indexOf("quasar2") >= 0) return "quasar2";
        if (_chgProject.indexOf("quasar") >= 0) return "quasar";
        if (_chgProject.indexOf("copper") >= 0) return "copper";
        return "";
    }

    // Firmware-change tracking — set when the GATT-reported FW differs
    // from the value persisted from the previous boot. Catches Wallbox
    // silent auto-OTAs that change behaviour overnight.
    bool firmwareChanged() const { return _fwChanged; }
    String previousFirmware() const { return _prevFw; }
    void dismissFirmwareChange() { _fwChanged = false; _prevFw = ""; }

private:
    void _connect();
    void _disconnect();
    bool _authenticate();
    static void _notifyCb(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);

    static WallboxBLE* _instance;

    NimBLEClient* _client = nullptr;
    NimBLERemoteCharacteristic* _chr = nullptr;

    String _addr;
    String _pin;
    String _svcUUID;
    String _chrUUID;          // write characteristic (also notify in single-char mode)
    String _txChrUUID;        // optional separate notify characteristic (Pulsar Plus)
    NimBLEAddress _foundAddr;
    State _state = State::DISCONNECTED;
    bool _pinRequired = false;

    // Response handling
    bapi::ResponseParser _parser;
    String _lastResponse;
    volatile bool _responseReady = false;
    ResponseCallback _responseCb = nullptr;
    YieldCallback _yieldCb = nullptr;

    // Command queue
    String _pendingCmd;
    bool _hasPending = false;

    // Reconnect timing
    uint32_t _lastConnectAttempt = 0;
    uint32_t _connectBackoff = 2000;

    // Pause window (for releasing BLE to official app)
    uint32_t _pausedUntil = 0;

    // Keepalive & activity tracking
    uint32_t _lastActivityTime = 0;
    static const uint32_t PING_INTERVAL_MS = 30000;

    // Scan cache for smart reconnect
    uint32_t _lastSeenTime = 0;
    static const uint32_t SCAN_CACHE_MS = 30000;

    // ---- Phase 1 BLE task refactor (rc15) ----
    // The state machine (scan / connect / auth / keepalive) and sendCommand
    // serialisation move onto a dedicated FreeRTOS task so the Arduino
    // main loop never blocks on BLE work. sendCommand() can still be called
    // from any task and remains synchronous, but only one BAPI command is
    // in flight at a time (serialised by _cmdMutex). The task itself drives
    // loop() ~50 Hz.
    SemaphoreHandle_t _cmdMutex = nullptr;
    TaskHandle_t      _taskHandle = nullptr;
    static void _taskFn(void* arg);

    // ---- Phase 2 polling on the BLE task (rc16) ----
    // The BLE task drives all periodic BAPI polling (status, realtime,
    // settings, meter, notifications). Results live in these cached
    // strings; each has a monotonic seq counter. The main task reads
    // seq vs its own "last published" counter and publishes when it
    // advances — so MQTT/WS publishing stays on main task (PubSubClient
    // is not thread-safe), but the BAPI traffic that drives it no
    // longer blocks main loop iterations.
    SemaphoreHandle_t _cacheMutex = nullptr;
    String   _cachedStatusJson, _cachedRealtimeJson, _cachedMeterJson;
    String   _cachedSettingsJson, _cachedNotificationsJson;
    uint32_t _seqStatus = 0,   _seqRealtime = 0, _seqMeter = 0;
    uint32_t _seqSettings = 0, _seqNotifications = 0;
    // Poll timers (BLE-task local)
    uint32_t _lastStatusPoll = 0, _lastRealtimePoll = 0, _lastNotifPoll = 0;
    uint32_t _statusPollMs = 10000, _realtimePollMs = 30000;
    static const uint32_t NOTIF_POLL_MS = 60000;
    void _pollStatus();
    void _pollRealtime();
    void _pollSettings();
    void _pollNotifications();
    void _storeCache(String& dst, uint32_t& seq, const String& value);

    // ---- Phase 3 BLE request queue (2.7.0 in progress) ----
    // Single-slot _pendingCmd is being replaced by a real FreeRTOS queue
    // so /api/command and MQTT _handleCommand can enqueue commands
    // without blocking on _cmdMutex. See docs/plans/2.7.0-api-command-async.md.
    // ReplyMode is in the public section above.
    struct BleReq {
        uint32_t  reqId;
        char      met[16];
        char      par[192];     // BAPI payloads are small; truncate-and-warn
        ReplyMode replyMode;
        TaskHandle_t waiter;    // xTaskNotify target, NULL if fire-and-forget
        uint32_t  enqueuedAt;   // millis() for timeout / drop-old logic
        uint32_t  bapiTimeoutMs;// step 9d: per-request BAPI timeout. 0 =
                                // use _sendCommandDirect default (5000 ms).
                                // Web handler sets this to match the caller's
                                // ?wait so the BAPI roundtrip doesn't give up
                                // earlier than the HTTP wait — that mismatch
                                // was causing 202s on slow methods like gupdc
                                // even though the caller would have waited.
    };
    // FreeRTOS queue handle, depth kBleReqQueueDepth.
    QueueHandle_t _reqQueue = nullptr;
    static const uint8_t kBleReqQueueDepth = 6;
    // Monotonic request ID counter. Bumped via __atomic_fetch_add
    // because enqueueRequest runs on both the main task (web handler)
    // and the BLE task (MQTT callback invoked from inside yield during
    // a sendCommand wait). Plain ++ would race and produce duplicate
    // ids; using _cmdMutex would serialise long BAPI roundtrips behind
    // a counter increment. Atomic RELAXED is lock-free, microsecond,
    // and correct under both visit orders.
    uint32_t _nextReqId = 1;

    // Step 3: response RAM map. Capped FIFO ring of {reqId, json}
    // pairs populated by the drain loop after sendCommand returns
    // and drained by tryFetchResponse(). Cap is intentionally small
    // — if a caller hasn't polled within ~4 subsequent requests
    // they probably aren't coming back, and the response would have
    // been delivered via MQTT/WS by step 5 anyway. Mutex-protected.
    struct ResponseSlot {
        uint32_t reqId;          // 0 = empty slot
        uint32_t completedAt;    // millis() when stored, for diag
        String   json;
    };
    ResponseSlot _responseMap[kResponseMapSize] = {};
    uint8_t      _responseMapHead = 0;  // next eviction index (FIFO)
    SemaphoreHandle_t _responseMapMutex = nullptr;
    // Internal: store a response in the map under _responseMapMutex.
    // No-op if reqId == 0 (fire-and-forget) or json is empty.
    void _storeResponse(uint32_t reqId, const String& json);

    // Step 5: pending-MQTT-publish ring. BLE-task drain enqueues
    // when replyMode == MQTT_PUBLISH; main task drains via
    // drainPendingResponsePub() and publishes through wallboxMQTT.
    // Capped so a wedged MQTT can't grow unbounded — drops oldest
    // with a log line.
    struct PendingPub {
        String met;
        String json;
    };
    PendingPub _pendingPub[kPendingPubSize] = {};
    uint8_t    _pendingPubHead = 0;   // next eviction (write) index
    uint8_t    _pendingPubTail = 0;   // next read index
    SemaphoreHandle_t _pendingPubMutex = nullptr;
    void _enqueueMqttPub(const String& met, const String& json);

    // 2.7.0 step 4: the "direct" BAPI write+wait path. Same body as
    // the pre-step-4 sendCommand: takes _cmdMutex, writes the framed
    // BAPI bytes to _chr, polls _responseReady. Used by the BLE-task-
    // internal callers (periodic polls, keepalive, post-connect
    // identity reads, PIN auth, drain loop). External callers go
    // through the public sendCommand() wrapper which queues + waits.
    String _sendCommandDirect(const char* met, const char* par = "null", uint32_t timeoutMs = 5000);

    // RSSI smoothing — NimBLE's getRssi() returns per-packet instantaneous
    // values that swing wildly (issue #6). Sample on a fixed cadence and
    // apply an EMA so all UI/MQTT/WS consumers see the same stable number.
    mutable int _rssiSmoothed = -127;
    mutable uint32_t _rssiLastSample = 0;
    static const uint32_t RSSI_SAMPLE_MS = 2000;

    // Raw-RX diagnostic state — only spams the log buffer before we see
    // the first valid BAPI frame on a fresh connection. Reset in _disconnect().
    bool _seenBapiThisConnection = false;

    // Stats
    uint32_t _txCount = 0;
    uint32_t _rxCount = 0;
    int _scanRSSI = -127;
    String _devMfg, _devModel, _devFw, _devName;
    String _chgSerial, _chgMac, _chgGrounding;
    String _chgAppFw;     // fw_v_.s — charger application FW (e.g. "6.11.16")
    String _chgProject;   // fw_v_.p — project name (e.g. "prj15-pulsar-max")
    int32_t _chgSessionCount = -1;  // r_ses.size — lifetime session counter
    int32_t _chgPowerBoost   = -1;  // r_hsh — household-meter current cap
    int32_t _chgLockState    = -1;  // r_lck — 0=unlocked, 1=locked
    String  _chgNetSsid;
    String  _chgNetIp;
    int     _chgNetSignal = 0;  // 0-100 quality % (gnsta.signal)
    // Set true once fw_v_ is read so the main loop can re-trigger HA
    // discovery with the real charger app FW in dev["sw_version"].
    bool    _discoveryStale = false;

public:
    // Last observed auto-lock timeout in minutes (g_alo, converted from
    // seconds). Defaults to 1 so toggling the HA Auto Lock switch ON
    // without ever having seen a non-zero value still produces a
    // sensible 1-minute window. Persisted in RAM across BLE polls; not
    // in NVS (no need — the next poll re-reads from the charger).
    // peter-mcc + benvanmierloo PR #9 follow-up.
    int     _lastAutolockMin = 1;
    int     lastAutolockMin() const { return _lastAutolockMin; }

private:
    String _chargerModel = "max";
    String _prevFw;
    bool _fwChanged = false;

    int _nextId = 1;
};

extern WallboxBLE wallboxBLE;
