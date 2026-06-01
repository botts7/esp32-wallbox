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

    // Send HA MQTT auto-discovery configs
    void sendDiscovery();

private:
    void _connect();
    void _subscribe();
    static void _mqttCallback(char* topic, byte* payload, unsigned int len);

    // Handle incoming command
    void _handleCommand(const char* subtopic, const char* payload);

    PubSubClient* _client = nullptr;
    uint32_t _lastConnectAttempt = 0;
    bool _discoveryPublished = false;
};

extern WallboxMQTT wallboxMQTT;
