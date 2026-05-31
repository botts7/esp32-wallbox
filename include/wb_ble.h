#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
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
    String _chargerModel = "max";
    String _prevFw;
    bool _fwChanged = false;

    int _nextId = 1;
};

extern WallboxBLE wallboxBLE;
