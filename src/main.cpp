#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include "wb_config.h"
#include "wb_ble.h"
#include "wb_mqtt.h"
#include "wb_web.h"
#include "wb_ws.h"
#include "bapi.h"
#include <ArduinoJson.h>

// ---- Polling timers ----
static uint32_t lastStatusPoll = 0;
static uint32_t lastRealtimePoll = 0;
static uint32_t lastGatewayPublish = 0;

// ---- WiFi ----
static bool connectWiFi() {
    const WBConfig& cfg = configMgr.get();
    if (cfg.wifiSSID.length() == 0) return false;

    Serial.printf("[WiFi] Connecting to %s", cfg.wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.wifiSSID.c_str(), cfg.wifiPass.c_str());

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        Serial.print(".");
        tries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("\n[WiFi] Failed to connect");
    return false;
}

static void checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Reconnecting...");
        WiFi.reconnect();
    }
}

// ---- Poll charger status ----
static String lastStatus, lastRealtime, lastMeter;

static void pollMeter() {
    if (!wallboxBLE.isConnected()) return;
    String resp = wallboxBLE.sendCommand(bapi::MET_GET_METER);
    if (!resp.isEmpty()) {
        lastMeter = resp;
        wallboxMQTT.publishResponse("meter", resp);
        wbws::broadcast("meter", resp);
    }
}

// Poll charger settings (auto lock, eco smart, power sharing, etc) and publish
// merged JSON to wallbox/settings for HA native entities
static void pollSettings() {
    if (!wallboxBLE.isConnected()) return;

    JsonDocument merged;

    String r1 = wallboxBLE.sendCommand(bapi::MET_GET_AUTOLOCK);
    if (!r1.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r1) == DeserializationError::Ok) {
            if (d["r"].is<JsonObject>()) {
                merged["autolock"] = d["r"]["enabled"] | 0;
                merged["autolock_time"] = d["r"]["time"] | 60;
            } else {
                merged["autolock"] = d["r"] | 0;
                merged["autolock_time"] = 60;
            }
        }
    }

    String r2 = wallboxBLE.sendCommand("g_ecos", "null");
    if (!r2.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r2) == DeserializationError::Ok) {
            merged["eco_mode"] = d["r"]["esm"] | 0;
            merged["eco_power"] = d["r"]["esp"] | 100;
        }
    }

    String r3 = wallboxBLE.sendCommand("g_psh", "null");
    if (!r3.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r3) == DeserializationError::Ok) {
            merged["power_sharing"] = d["r"]["dyps"] | 0;
        }
    }

    String r4 = wallboxBLE.sendCommand("g_phsw", "null");
    if (!r4.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r4) == DeserializationError::Ok) {
            merged["phase_switch"] = d["r"]["enabled"] | 0;
        }
    }

    String r5 = wallboxBLE.sendCommand(bapi::MET_GET_TIMEZONE);
    if (!r5.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r5) == DeserializationError::Ok) {
            const char* tz = d["r"]["timezone"] | "UTC";
            merged["timezone"] = tz;
        }
    }

    // Halo — just a placeholder since we don't have a verified getter yet
    merged["halo"] = 2;  // Default to Medium

    String out;
    serializeJson(merged, out);
    wallboxMQTT.publishSettings(out);
    wbws::broadcast("settings", out);
}

static void pollStatus() {
    if (!wallboxBLE.isConnected()) return;
    String resp = wallboxBLE.sendCommand(bapi::MET_GET_STATUS);
    if (!resp.isEmpty()) {
        lastStatus = resp;
        wallboxMQTT.publishStatus(resp);
        webServer.updateCache(lastStatus, lastRealtime);
        wbws::broadcast("status", resp);
    }
    // Energy meter on same cycle (lightweight, useful for live monitoring)
    pollMeter();
}

static void pollRealtime() {
    if (!wallboxBLE.isConnected()) return;
    String resp = wallboxBLE.sendCommand(bapi::MET_GET_REALTIME);
    if (!resp.isEmpty()) {
        lastRealtime = resp;
        wallboxMQTT.publishRealtime(resp);
        webServer.updateCache(lastStatus, lastRealtime);
    }
}

static void publishGatewayInfo() {
    if (!wallboxMQTT.isConnected()) return;
    String json = "{\"rssi\":";
    json += String(wallboxBLE.rssi());
    json += ",\"ble_state\":\"";
    json += wallboxBLE.stateStr();
    json += "\",\"tx\":";
    json += String(wallboxBLE.txCount());
    json += ",\"rx\":";
    json += String(wallboxBLE.rxCount());
    json += ",\"uptime\":";
    json += String(millis() / 1000);
    json += ",\"heap\":";
    json += String(ESP.getFreeHeap());
    json += ",\"wifi_rssi\":";
    json += String(WiFi.RSSI());
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"dev_mfg\":\"" + wallboxBLE.deviceManufacturer() + "\"";
    json += ",\"dev_model\":\"" + wallboxBLE.deviceModel() + "\"";
    json += ",\"dev_fw\":\"" + wallboxBLE.deviceFirmware() + "\"";
    json += ",\"dev_name\":\"" + wallboxBLE.deviceName() + "\"";
    json += "}";
    wallboxMQTT.publishResponse("gateway", json);
}

// ---- Setup & Loop ----

void setup() {
    Serial.begin(115200);
    delay(500);

    // Smart rollback — don't validate until WiFi connects successfully
    // If firmware is broken and can't connect, bootloader reverts after 3 failed boots

    Serial.println("\n============================");
    Serial.println("  Wallbox BLE Gateway v1.0");
    Serial.println("============================\n");

    // Log OTA partition info
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        Serial.printf("[OTA] Running from: %s (0x%x)\n", running->label, running->address);
    }

    // Load config from NVS
    configMgr.begin();

    // Init BLE
    NimBLEDevice::init("WallboxGW");
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9dBm max range
    Serial.println("[BLE] TX power: +9 dBm");

    // Check if WiFi is configured
    if (!configMgr.hasWiFi()) {
        // First boot — AP mode for setup
        Serial.println("[Main] No WiFi configured — starting AP setup mode");
        webServer.beginAP();
        return;  // loop() will only run web server
    }

    // ---- Radio coexistence (WiFi + BLE share 2.4GHz) ----
    esp_wifi_set_ps(WIFI_PS_NONE);              // no WiFi power save — prevents BLE stalls
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);  // fair share between WiFi and BLE
    Serial.println("[Radio] Coexistence: BALANCE, WiFi PS: OFF");

    // Try connecting to WiFi
    if (!connectWiFi()) {
        // WiFi failed — start AP mode so user can fix settings
        Serial.println("[Main] WiFi failed — starting AP mode for reconfiguration");
        webServer.beginAP();
        return;
    }

    // WiFi connected — mark OTA partition as valid (smart rollback)
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[OTA] Firmware validated (WiFi OK)");

    // mDNS — access at http://wallbox-gw.local
    // Retry a few times as mDNS init sometimes fails on first attempt
    bool mdnsOk = false;
    for (int i = 0; i < 3 && !mdnsOk; i++) {
        mdnsOk = MDNS.begin("wallbox-gw");
        if (!mdnsOk) delay(500);
    }
    if (mdnsOk) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "device", "wallbox-gateway");
        MDNS.addServiceTxt("http", "tcp", "version", "v2.0");
        MDNS.addServiceTxt("http", "tcp", "path", "/");
        Serial.printf("[mDNS] http://wallbox-gw.local (IP: %s)\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[mDNS] Failed to start — use IP address directly");
    }

    // WiFi connected — start services
    webServer.beginSTA();
    wallboxMQTT.begin();
    wbws::begin();

    // OTA updates — hostname identifies this device on the network
    ArduinoOTA.setHostname("wallbox-gw");
    // Use web auth password if set, otherwise generate per-device from MAC
    const WBConfig& cfg = configMgr.get();
    String otaPass;
    if (cfg.authPass.length() > 0) {
        otaPass = cfg.authPass;
    } else {
        uint8_t mac[6]; WiFi.macAddress(mac);
        char buf[13];
        snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        otaPass = "wb-" + String(buf).substring(6);  // device-unique default
    }
    ArduinoOTA.setPassword(otaPass.c_str());
    Serial.printf("[OTA] Password: %s (use web auth pass, or shown here for espota)\n", otaPass.c_str());
    ArduinoOTA.onStart([]() {
        Serial.println("[OTA] Update starting...");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] Update complete!");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[OTA] %u%%\r", (progress * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error: %u\n", error);
    });
    ArduinoOTA.begin();
    Serial.println("[OTA] Ready");

    if (configMgr.hasBLE()) {
        const WBConfig& cfg = configMgr.get();
        wallboxBLE.begin(cfg.bleAddr.c_str());

        // Yield callback — keeps web server + OTA responsive during BLE waits
        wallboxBLE.onYield([]() {
            webServer.loop();
            ArduinoOTA.handle();
        });
        if (cfg.bleService.length() > 0 && cfg.bleChar.length() > 0) {
            wallboxBLE.setUUIDs(cfg.bleService.c_str(), cfg.bleChar.c_str());
        }
        if (cfg.blePin.length() > 0) {
            wallboxBLE.setPin(cfg.blePin.c_str());
        }
    } else {
        Serial.println("[Main] No BLE address configured — set via web UI or MQTT");
    }

    Serial.println("[Main] Setup complete\n");
}

void loop() {
    // Always run web server + OTA
    ArduinoOTA.handle();
    webServer.loop();

    // If in AP-only mode (no WiFi configured), just serve the portal
    if (!configMgr.hasWiFi() || WiFi.status() != WL_CONNECTED) {
        delay(10);
        return;
    }

    // Check WiFi
    static uint32_t lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 30000) {
        lastWifiCheck = millis();
        checkWiFi();
    }

    // Run MQTT loop
    wallboxMQTT.loop();

    // Run WS loop (handle client connects/disconnects + frames)
    wbws::loop();

    // Broadcast BLE health every 5s to any connected WS clients
    static uint32_t lastHealth = 0;
    if (millis() - lastHealth >= 5000 && wbws::clientCount() > 0) {
        lastHealth = millis();
        wbws::broadcastBleHealth(wallboxBLE.stateStr(), wallboxBLE.rssi(),
                                 wallboxBLE.lastActivityAge() / 1000);
    }

    // Run BLE loop — skip during OTA to free radio + CPU
    extern bool otaInProgress;
    if (configMgr.hasBLE() && !otaInProgress) {
        wallboxBLE.loop();
    }

    // Track BLE state for availability + immediate poll on reconnect
    static bool wasConnected = false;
    static uint32_t bleDisconnectedAt = 0;
    bool nowConnected = wallboxBLE.isConnected();

    if (nowConnected && !wasConnected) {
        // Just reconnected — poll immediately
        lastStatusPoll = 0;
        lastRealtimePoll = 0;
        bleDisconnectedAt = 0;
        wallboxMQTT.publishAvailability(true);
    } else if (!nowConnected && wasConnected) {
        // Just disconnected — start grace timer
        bleDisconnectedAt = millis();
    }
    wasConnected = nowConnected;

    // Availability: online when BLE connected, offline after 60s disconnected
    static uint32_t lastAvailPublish = 0;
    if (wallboxMQTT.isConnected() && millis() - lastAvailPublish >= 30000) {
        lastAvailPublish = millis();
        bool avail = nowConnected || (bleDisconnectedAt > 0 && millis() - bleDisconnectedAt < 60000);
        wallboxMQTT.publishAvailability(avail);
    }

    // Poll charger status periodically
    const WBConfig& cfg = configMgr.get();
    if (nowConnected && wallboxMQTT.isConnected()) {
        uint32_t now = millis();

        if (now - lastStatusPoll >= cfg.statusPollMs) {
            lastStatusPoll = now;
            pollStatus();
        }

        if (now - lastRealtimePoll >= cfg.realtimePollMs) {
            lastRealtimePoll = now;
            pollRealtime();
            pollSettings();  // poll settings every realtime cycle (30s default)
        }

        if (now - lastGatewayPublish >= 60000) {
            lastGatewayPublish = now;
            publishGatewayInfo();
        }
    }

    delay(10);
}
