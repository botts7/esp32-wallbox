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

    // RSSI smoothing — NimBLE's getRssi() returns per-packet instantaneous
    // values that swing wildly (issue #6). Sample on a fixed cadence and
    // apply an EMA so all UI/MQTT/WS consumers see the same stable number.
    mutable int _rssiSmoothed = -127;
    mutable uint32_t _rssiLastSample = 0;
    static const uint32_t RSSI_SAMPLE_MS = 2000;

    // Stats
    uint32_t _txCount = 0;
    uint32_t _rxCount = 0;
    int _scanRSSI = -127;
    String _devMfg, _devModel, _devFw, _devName;
    String _chgSerial, _chgMac, _chgGrounding;
    String _chargerModel = "max";
    String _prevFw;
    bool _fwChanged = false;

    int _nextId = 1;
};

extern WallboxBLE wallboxBLE;
