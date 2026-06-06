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
#include "wb_log.h"
#include "wb_health.h"
#include "wb_diag.h"
#include "wb_net.h"
#include "wb_version.h"
#include "bapi.h"
#include <ArduinoJson.h>

// ---- Polling timers ----
// Phase 2 (rc16): periodic BAPI polling moved into the BLE task. Only
// publishGatewayInfo() — pure WiFi/diag data, no BLE — stays here.
static uint32_t lastGatewayPublish = 0;

// ---- WiFi ----
// connectWiFi() and checkWiFi() moved to src/wb_net.cpp in 2.7.0.
// The sync 30 s checkWiFi() poll is replaced by an event-driven
// state machine in wb_net::tick() that only calls WiFi.reconnect()
// when the driver's own auto-reconnect has clearly given up.

// ---- Last-published-seq tracking ----
// Phase 2 (rc16): all periodic BAPI polling now runs on the BLE task
// (see wb_ble.cpp _pollStatus/_pollRealtime/_pollSettings/etc.). The
// main loop's job is to notice when the BLE task has updated a cache
// and publish the new value to MQTT/WS. These counters remember what
// seq we last published so we only publish on advances.
static String lastStatus, lastRealtime;  // kept around for webServer.updateCache contract
static uint32_t _lastSeqStatus = 0, _lastSeqRealtime = 0, _lastSeqMeter = 0;
static uint32_t _lastSeqSettings = 0, _lastSeqNotifications = 0;

static void publishGatewayInfo();  // forward decl — defined below

static void publishCachedStatusIfNew() {
    String resp; uint32_t seq = 0;
    wallboxBLE.copyCachedStatus(resp, seq);
    if (seq != 0 && seq != _lastSeqStatus && !resp.isEmpty()) {
        _lastSeqStatus = seq;
        lastStatus = resp;
        wallboxMQTT.publishStatus(resp);
        wallboxMQTT.publishCarConnected(lastStatus, lastRealtime);
        webServer.updateCache(lastStatus, lastRealtime);
        wbws::broadcast("status", resp);
    }
}
static void publishCachedRealtimeIfNew() {
    String resp; uint32_t seq = 0;
    wallboxBLE.copyCachedRealtime(resp, seq);
    if (seq != 0 && seq != _lastSeqRealtime && !resp.isEmpty()) {
        _lastSeqRealtime = seq;
        lastRealtime = resp;
        wallboxMQTT.publishRealtime(resp);
        wallboxMQTT.publishCarConnected(lastStatus, lastRealtime);
        webServer.updateCache(lastStatus, lastRealtime);
    }
}
static void publishCachedMeterIfNew() {
    String resp; uint32_t seq = 0;
    wallboxBLE.copyCachedMeter(resp, seq);
    if (seq != 0 && seq != _lastSeqMeter && !resp.isEmpty()) {
        _lastSeqMeter = seq;
        wallboxMQTT.publishResponse("meter", resp);
        wbws::broadcast("meter", resp);
    }
}
static void publishCachedSettingsIfNew() {
    String resp; uint32_t seq = 0;
    wallboxBLE.copyCachedSettings(resp, seq);
    if (seq != 0 && seq != _lastSeqSettings && !resp.isEmpty()) {
        _lastSeqSettings = seq;
        wallboxMQTT.publishSettings(resp);
        wbws::broadcast("settings", resp);
    }
}
static void publishCachedNotificationsIfNew() {
    String resp; uint32_t seq = 0;
    wallboxBLE.copyCachedNotifications(resp, seq);
    if (seq != 0 && seq != _lastSeqNotifications && !resp.isEmpty()) {
        _lastSeqNotifications = seq;
        wallboxMQTT.publishResponse("notifications", resp);
    }
}

// Globals defined in wb_web.cpp — exposed for MQTT diagnostic publish.
// Keeping them as plain extern (no header) since this is the only other
// translation unit that needs them, and the rc21 reentry tripwire is
// already documented in handleApiCommand.
extern volatile int g_webMaxReentry;
extern volatile uint32_t g_loopLastMs;
extern volatile uint32_t g_loopMaxMs;
int wb_web_tokens_remaining();

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
    // Min-ever heap is the fragmentation watermark. HA can graph this to
    // spot a slow leak before it hits the panic threshold.
    json += ",\"heap_min_ever\":";
    json += String((uint32_t)esp_get_minimum_free_heap_size());
    json += ",\"wifi_rssi\":";
    json += String(WiFi.RSSI());
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    // Gateway-side firmware version (gateway, not the BLE module) — so HA
    // can show which build is running and alert on unexpected downgrade.
    json += ",\"fw\":\"" WB_VERSION "\"";
    // rc21 reentry tripwire + token bucket — proof fields surfaced over
    // MQTT so HA can alarm if max_reentry ever rises above 1 (regression)
    // and graph tokens-remaining for rate-limit pressure.
    json += ",\"max_reentry\":";
    json += String((int)g_webMaxReentry);
    json += ",\"tokens\":";
    json += String(wb_web_tokens_remaining());
    // Longest main-loop iteration gap since boot — wedge tripwire.
    // Same shape as max_reentry. Healthy: < ~200 ms. Multi-second
    // values mean the main task was blocked (peter-mcc #4 wedge class).
    json += ",\"loop_max_ms\":";
    json += String((uint32_t)g_loopMaxMs);
    // Boot reason + per-firmware bad-boot count — same data /info badge
    // shows, now visible from HA. Useful for fleet monitoring.
    json += ",\"boot_reason\":\"";
    json += wb_health::currentBootReasonStr();
    json += "\"";
    // BLE pause state — when the user "Released BLE for App", HA users
    // need to know polling has stopped (sensors will stale) and roughly
    // how long until it resumes.
    json += ",\"ble_paused\":";
    json += (wallboxBLE.isPaused() ? "true" : "false");
    json += ",\"ble_pause_remaining_s\":";
    json += String(wallboxBLE.pauseRemainingMs() / 1000);
    json += ",\"chg_grounding\":\"" + wallboxBLE.chargerGrounding() + "\"";
    // Charger application firmware (the version Wallbox app shows) +
    // canonical project name from fw_v_ BAPI. Distinct from dev_fw
    // (the BLE radio module's firmware) — peter-mcc #4 flagged the
    // confusion when our HA "BLE Firmware" label was being compared
    // to the Wallbox app's charger-firmware number.
    json += ",\"chg_app_fw\":\"" + wallboxBLE.chargerAppFirmware() + "\"";
    json += ",\"chg_project\":\"" + wallboxBLE.chargerProject() + "\"";
    {
        int32_t sc = wallboxBLE.chargerSessionCount();
        json += sc >= 0 ? (",\"chg_sessions\":" + String((int)sc))
                        : ",\"chg_sessions\":null";
    }
    json += ",\"chg_power_boost\":" + String((int)wallboxBLE.chargerPowerBoost());
    json += ",\"chg_lock_state\":" + String((int)wallboxBLE.chargerLockState());
    json += ",\"chg_net_ssid\":\"" + wallboxBLE.chargerNetworkSsid() + "\"";
    json += ",\"chg_net_ip\":\"" + wallboxBLE.chargerNetworkIp() + "\"";
    json += ",\"chg_net_signal\":" + String(wallboxBLE.chargerNetworkSignal());
    json += ",\"dev_mfg\":\"" + wallboxBLE.deviceManufacturer() + "\"";
    json += ",\"dev_model\":\"" + wallboxBLE.deviceModel() + "\"";
    json += ",\"dev_fw\":\"" + wallboxBLE.deviceFirmware() + "\"";
    json += ",\"dev_name\":\"" + wallboxBLE.deviceName() + "\"";
    json += ",\"chg_sn\":\"" + wallboxBLE.chargerSerial() + "\"";
    json += ",\"chg_mac\":\"" + wallboxBLE.chargerMac() + "\"";
    json += "}";
    wallboxMQTT.publishResponse("gateway", json);
}

// ---- Setup & Loop ----

void setup() {
    Serial.begin(115200);
    delay(500);

    // Smart rollback — don't validate until WiFi connects successfully
    // If firmware is broken and can't connect, bootloader reverts after 3 failed boots

    Log.println("\n============================");
    Log.printf("  Wallbox BLE Gateway %s\n", WB_VERSION);
    Log.println("============================\n");

    // Log OTA partition info
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        Log.printf("[OTA] Running from: %s (0x%x)\n", running->label, running->address);
    }

    // Capture esp_reset_reason() — tells us whether the previous run
    // ended cleanly (software / power-on) or violently (panic / WDT /
    // brownout). Persisted to NVS so we can read it later even if the
    // log buffer wraps before the user gets to /logs.
    wb_health::recordBootReason();

    // Bump the boot counter BEFORE anything else might crash. If we've
    // tried to boot this firmware too many times without reaching a
    // healthy state, something's wrong — log a warning. (Forcing a
    // partition swap from here is unsafe at this point in startup; we
    // just surface the issue so the user/installer sees it.)
    uint8_t bootN = wb_health::bootCountBumpAndRead();
    Log.printf("[Health] Boot attempt #%u for this firmware\n", bootN);
    if (bootN >= wb_health::BOOT_FAIL_THRESHOLD) {
        Log.printf("[Health] WARNING: %u failed-to-be-healthy boots in a row — current firmware is suspect\n", bootN);
    }

    // Load config from NVS
    configMgr.begin();

    // Init BLE
    NimBLEDevice::init("WallboxGW");
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9dBm max range
    NimBLEDevice::setSecurityAuth(true, true, true);
    // IO capability — KeyboardOnly handles both eras of both models.
    // Newer Wallbox firmware (MAX 6.11.26+, Plus 6.7.x+) shows a "Bluetooth
    // Passcode" in the phone app; the user copies it into our PIN field
    // and we answer onPassKeyRequest() with it. Older firmware without a
    // passkey: the BLE spec auto-negotiates to Just Works when the
    // peripheral declares NoInputNoOutput, so KeyboardOnly is harmless there.
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    Log.println("[BLE] TX power: +9 dBm, IO cap: KeyboardOnly");

    // Check if WiFi is configured
    if (!configMgr.hasWiFi()) {
        // First boot — AP mode for setup
        Log.println("[Main] No WiFi configured — starting AP setup mode");
        webServer.beginAP();
        return;  // loop() will only run web server
    }

    // ---- Radio coexistence (WiFi + BLE share 2.4GHz) ----
    esp_wifi_set_ps(WIFI_PS_NONE);              // no WiFi power save — prevents BLE stalls
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);  // fair share between WiFi and BLE
    Log.println("[Radio] Coexistence: BALANCE, WiFi PS: OFF");

    // Try connecting to WiFi via the wb_net module. wb_net::begin()
    // does the initial blocking connect (~20 s timeout, same as
    // pre-2.7.0) AND registers the event-driven reconnect machinery.
    if (!wb_net::begin()) {
        // WiFi failed — start AP mode so user can fix settings
        Log.println("[Main] WiFi failed — starting AP mode for reconfiguration");
        webServer.beginAP();
        return;
    }

    // WiFi connected — mark OTA partition as valid (smart rollback)
    esp_ota_mark_app_valid_cancel_rollback();
    Log.println("[OTA] Firmware validated (WiFi OK)");

    // Kick off SNTP. Used by wb_health::updateBootTimeIfPossible() so
    // boot-history entries get a real wall-clock timestamp once sync
    // lands (typically ~1–3s after WiFi up). Pool.ntp.org is the
    // canonical anycast resolver — no further config needed for UTC.
    configTime(0, 0, "pool.ntp.org");

    // mDNS — wb_net handles it now: initial bind happens on the
    // first GOT_IP via wb_net::tick() pending-flag drain, and the
    // same path also rebinds after a reconnect so the responder +
    // service records stay current with the (possibly new) IP.
    // Drive one tick here so the initial bind fires before
    // webServer.beginSTA / OTA — both expect mDNS to be up.
    wb_net::tick();

    // WiFi connected — start services
    webServer.beginSTA();
    wallboxMQTT.begin();
    wbws::begin();
    Log.begin();

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
    Log.printf("[OTA] Password: %s (use web auth pass, or shown here for espota)\n", otaPass.c_str());
    ArduinoOTA.onStart([]() {
        Log.println("[OTA] Update starting...");
    });
    ArduinoOTA.onEnd([]() {
        Log.println("[OTA] Update complete!");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Log.printf("[OTA] %u%%\r", (progress * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Log.printf("[OTA] Error: %u\n", error);
    });
    ArduinoOTA.begin();
    Log.println("[OTA] Ready");

    if (configMgr.hasBLE()) {
        const WBConfig& cfg = configMgr.get();
        wallboxBLE.begin(cfg.bleAddr.c_str());

        // Yield callback — runs while BLE is waiting on a BAPI response so
        // the rest of the gateway stays responsive. Includes wallboxMQTT.loop()
        // so PubSubClient's keepalive logic still gets cycles during long
        // settings polls — without this, several back-to-back BLE waits could
        // starve MQTT for 15+ seconds and PubSubClient would close the socket
        // (the "connection closed by client every 85s" pattern observed in the
        // rc12 broker log).
        //
        // CRITICAL: do NOT call webServer.loop() here. This callback fires from
        // inside sendCommand(), which is itself reached from a web request
        // handler (handleApiCommand) running inside http.handleClient().
        // Arduino WebServer is NOT re-entrant — it holds the in-flight request
        // as member state (_currentClient, parser position, _responseHeaders).
        // Pumping handleClient() re-entrantly accepts a second connection,
        // clobbers _currentClient, and the outer handler's http.send() then
        // writes to a freed socket → panic (LoadProhibited) under overlapping
        // requests. MQTT + OTA are re-entrancy-safe here; the web server is not.
        wallboxBLE.onYield([]() {
            ArduinoOTA.handle();
            wallboxMQTT.loop();
        });
        if (cfg.bleService.length() > 0 && cfg.bleChar.length() > 0) {
            // Dual-char mode if bleTxChar is set (e.g. Pulsar Plus); otherwise single-char (MAX default)
            wallboxBLE.setUUIDs(cfg.bleService.c_str(), cfg.bleChar.c_str(),
                                cfg.bleTxChar.c_str());
        }
        wallboxBLE.setChargerModel(cfg.chargerModel.c_str());
        wallboxBLE.setStatusPollMs(cfg.statusPollMs);
        wallboxBLE.setRealtimePollMs(cfg.realtimePollMs);
        if (cfg.blePin.length() > 0) {
            wallboxBLE.setPin(cfg.blePin.c_str());
        }
    } else {
        Log.println("[Main] No BLE address configured — set via web UI or MQTT");
    }

    Log.println("[Main] Setup complete\n");
}

void loop() {
    // Loop-iteration gap tracker — see g_loopMaxMs in wb_web.cpp.
    // First iteration's gap is meaningless (no prior timestamp), so
    // skip it via the !=0 check. Unsigned arithmetic handles millis()
    // wraparound at ~49 d correctly.
    //
    // Three things are excluded from the metric, in order:
    //
    // 1. The first 60 s of uptime — boot-phase MQTT discovery floods
    //    out ~30 sync publishes that block the main task for 3-5 s,
    //    which isn't the kind of *runtime* wedge this tripwire was
    //    built to detect (peter-mcc #4).
    // 2. A 30 s grace window after any tracked BLE or MQTT reconnect
    //    (wb_diag::loopMaxGateActive). Sync PubSubClient::connect()
    //    can block for ~15 s per attempt; a transient broker hiccup
    //    easily produces a 30-40 s gap that would otherwise saturate
    //    the metric. Surfaced by peter-mcc in 2.4.2 follow-up — his
    //    gateway showed 40 082 ms while two MQTT disconnects sat
    //    quietly in the reconnect counters.
    // 3. (Implicit) the first iteration with no prior timestamp.
    //
    // After all three filters, any gap that lands in the metric is a
    // genuine unprovoked wedge worth investigating.
    {
        uint32_t now = millis();
        // BOTH endpoints of the gap must be past the 60 s boot phase.
        // Earlier we only checked `now > 60000`, which meant a gap that
        // STRADDLED the boundary (boot-phase work still running at
        // 60 s + 1 ms) recorded the entire 4-5 s boot block as the
        // first steady-state measurement. peter-mcc 2.5.0 saw a
        // loop_max_ms=4156 right after boot from this bug.
        // Also gate on the post-reconnect grace window.
        if (g_loopLastMs > 60000 && !wb_diag::loopMaxGateActive(now)) {
            uint32_t gap = now - g_loopLastMs;
            if (gap > g_loopMaxMs) g_loopMaxMs = gap;
        }
        g_loopLastMs = now;
    }

    // Always run web server + OTA + telnet
    ArduinoOTA.handle();
    webServer.loop();
    Log.loop();

    // If in AP-only mode (no WiFi configured), just serve the portal.
    // Use wb_net's cached state here — cheaper than WiFi.status() and
    // tracks the GOT_IP/STA_DISCONNECTED events from the driver.
    if (!configMgr.hasWiFi() || !wb_net::isConnected()) {
        delay(10);
        return;
    }

    // Drive the WiFi event-driven state machine: drains any pending
    // GOT_IP / STA_DISCONNECTED events, runs mDNS refresh, and only
    // fires WiFi.reconnect() if the driver's own auto-reconnect has
    // clearly given up. Cheap (~10 us) when nothing is pending.
    wb_net::tick();

    // Run MQTT loop
    wallboxMQTT.loop();

    // Run WS loop (handle client connects/disconnects + frames)
    wbws::loop();

    // Mark healthy once WiFi is connected AND we've been up > 30s.
    // Healthy = clears the boot counter and tells ESP OTA layer the
    // partition is good (no rollback on next boot). Only fires once.
    if (!wb_health::isHealthy() && WiFi.status() == WL_CONNECTED && millis() > 30000) {
        wb_health::markHealthy();
    }

    // Patch the current boot record's wall-clock timestamp once NTP
    // has actually synced. Slow cadence — once a minute is plenty,
    // since updateBootTimeIfPossible() is a no-op after the first
    // successful patch.
    static uint32_t lastBootTimePatch = 0;
    if (millis() - lastBootTimePatch >= 60000) {
        lastBootTimePatch = millis();
        wb_health::updateBootTimeIfPossible();
    }

    // Broadcast BLE health every 5s to any connected WS clients
    static uint32_t lastHealth = 0;
    if (millis() - lastHealth >= 5000 && wbws::clientCount() > 0) {
        lastHealth = millis();
        wbws::broadcastBleHealth(wallboxBLE.stateStr(), wallboxBLE.rssi(),
                                 wallboxBLE.lastActivityAge() / 1000);
    }

    // BLE state machine runs on its own FreeRTOS task (wb_ble.cpp _taskFn,
    // spawned by wallboxBLE.begin()). Main loop no longer drives it — so
    // BLE scans and connects can't freeze the web UI / MQTT / WS handling.
    // We still pause the task during OTA via wallboxBLE.pause() which the
    // OTA upload handler already calls at FILE_START.

    // Track BLE state for availability + immediate poll on reconnect
    static bool wasConnected = false;
    static uint32_t bleDisconnectedAt = 0;
    bool nowConnected = wallboxBLE.isConnected();

    if (nowConnected && !wasConnected) {
        // Just reconnected — the BLE task's own poll timers will fire
        // on their natural cadence; no main-side poll-timer reset needed
        // in rc16 since main no longer drives the polls.
        bleDisconnectedAt = 0;
        wallboxMQTT.publishAvailability(true);
        wb_diag::reportReconnect(wb_diag::Kind::BLE);
    } else if (!nowConnected && wasConnected) {
        // Just disconnected — start grace timer
        bleDisconnectedAt = millis();
        wb_diag::reportDisconnect(wb_diag::Kind::BLE);
    }
    wasConnected = nowConnected;

    // Availability: online when BLE connected, offline after 60s disconnected
    static uint32_t lastAvailPublish = 0;
    if (wallboxMQTT.isConnected() && millis() - lastAvailPublish >= 30000) {
        lastAvailPublish = millis();
        bool avail = nowConnected || (bleDisconnectedAt > 0 && millis() - bleDisconnectedAt < 60000);
        wallboxMQTT.publishAvailability(avail);
    }

    // Phase 2 (rc16): periodic BAPI polling runs on the BLE FreeRTOS task
    // (see wb_ble.cpp _pollStatus/_pollRealtime/_pollSettings/_pollNotifications).
    // Main loop's job is to publish to MQTT/WS when the BLE task has fresh
    // data. Each check is a mutex-protected copy-out + seq compare; ~5 µs
    // each when the cache hasn't advanced. Never blocks on BLE.
    if (wallboxMQTT.isConnected()) {
        // Re-publish HA discovery when BLE init finishes reading fw_v_,
        // so the HA Device page's sw_version flips from the WB_VERSION
        // fallback to the real charger app firmware. One-shot — the
        // flag is cleared immediately so we don't republish every loop.
        // peter-mcc 2.4.1 follow-up.
        if (wallboxBLE.discoveryStale()) {
            wallboxBLE.clearDiscoveryStale();
            wallboxMQTT.sendDiscovery();
        }
        // Drain at most one MQTT discovery entity per loop iteration.
        // sendDiscovery() above arms the state machine; tickDiscovery()
        // publishes one entity per call until the burst is done. This
        // bounds the per-loop cost to ONE publish during a wedged-broker
        // event instead of compounding ~60 timeouts in series (peter-mcc
        // 2.5.1 observed 80,000 ms loop_max_ms overnight from a single
        // sendDiscovery against an MQTT broker that briefly stalled).
        wallboxMQTT.tickDiscovery();
        // 2.7.0 step 5: drain any pending MQTT-publish responses that
        // the BLE task queued. We can publish from here because the
        // main task owns PubSubClient. Drain to empty per iteration —
        // each publish is bounded by the 2.6.0 1s wifiClient socket
        // timeout, and the ring caps at kPendingPubSize entries, so
        // the loop bound is ~4 × 1 s worst case (unlikely).
        {
            String pubMet, pubJson;
            while (wallboxBLE.drainPendingResponsePub(pubMet, pubJson)) {
                wallboxMQTT.publishResponse(pubMet.c_str(), pubJson);
            }
        }
        publishCachedStatusIfNew();
        publishCachedRealtimeIfNew();
        publishCachedMeterIfNew();
        publishCachedSettingsIfNew();
        publishCachedNotificationsIfNew();

        uint32_t now = millis();
        if (now - lastGatewayPublish >= 60000) {
            lastGatewayPublish = now;
            publishGatewayInfo();
        }
    }

    delay(10);
}
