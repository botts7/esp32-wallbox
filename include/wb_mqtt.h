#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

// MQTT bridge: publishes charger state, subscribes to commands,
// sends HA auto-discovery configs.

class WallboxMQTT {
public:
    void begin();
    void loop();  // call from main loop
    bool isConnected() const;

    // Publish charger state (call after each BLE poll)
    void publishStatus(const String& json);
    void publishRealtime(const String& json);
    // Derived "car connected" state — needs both r_dat (st) and r_sta
    // (charger_status), which live on separate topics, so it's computed here.
    void publishCarConnected(const String& statusJson, const String& realtimeJson);
    void publishSettings(const String& json);
    void publishAvailability(bool online);

    // Publish raw BAPI response on a per-method topic
    void publishResponse(const char* method, const String& json);

    // Arm the HA auto-discovery state machine. Returns immediately
    // (~O(1)) — the actual ~60 sync MQTT publishes are spread across
    // subsequent main-loop iterations via tickDiscovery(). Previously
    // this function ran all publishes inline, which on an unhealthy
    // broker could block the main loop for tens of seconds while TCP
    // writes hit their socket timeouts in series. peter-mcc 2.5.1
    // observation: 80 s loop_max_ms overnight.
    void sendDiscovery();

    // Publish AT MOST ONE discovery entity per call. Drives the state
    // machine armed by sendDiscovery(). Idempotent and no-op once the
    // burst is complete (or while MQTT is disconnected). Called once
    // per main loop iteration from main.cpp.
    void tickDiscovery();

private:
    void _connect();
    void _subscribe();
    static void _mqttCallback(char* topic, byte* payload, unsigned int len);

    // 3.0 task #77 — table-driven discovery dispatcher. Built when
    // WB_DISCOVERY_TABLE_DRIVEN is non-zero; called from tickDiscovery()
    // in place of the legacy 57-case switch. The table itself + the
    // enums + the resolver live as file-static state in wb_mqtt.cpp
    // since they're closely tied to its internals and exposing them
    // outside the translation unit would force the entity catalogue
    // into the header. See docs/plans/3.0-discovery-table.md.
    void _tickDiscoveryFromTable(size_t index);

    // Handle incoming command
    void _handleCommand(const char* subtopic, const char* payload);

    PubSubClient* _client = nullptr;
    uint32_t _lastConnectAttempt = 0;
    bool _discoveryPublished = false;
    bool _wasConnected = false;  // edge-trigger for wb_diag counters

    // Discovery state machine. SIZE_MAX = idle/complete; 0..N-1 = next
    // entity to publish on the next tickDiscovery() call.
    size_t _discoveryIndex = SIZE_MAX;
    // Per-arm topic cache. Populated once in sendDiscovery() from
    // configMgr/baseTopic()/cmdPrefix(); read by tickDiscovery() cases.
    // Lifting these out of the per-iteration switch means we don't
    // re-walk configMgr or rebuild the same Strings 60 times.
    struct DiscoveryTopics {
        String sTopic, rTopic, gTopic, mTopic, nTopic, setTopic;
        String cmdCurrent, cmdCharging, cmdLock, cmdReboot;
        String cmdAutolockEnable, cmdAutolockTime;
        String cmdEcoMode, cmdEcoPower;
        String cmdPowerShare, cmdPhaseSwitch, cmdHalo, cmdTimezone;
        String cmdResumeSched;
    } _discTopics;
};

extern WallboxMQTT wallboxMQTT;
