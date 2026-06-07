#include "wb_ble.h"
#include "wb_log.h"
#include "wb_config.h"
#include <ArduinoJson.h>
#include <esp_coexist.h>

WallboxBLE wallboxBLE;
WallboxBLE* WallboxBLE::_instance = nullptr;

static const char* DEF_SVC = "2456e1b9-26e2-8f83-e744-f34f01e9d701";
static const char* DEF_CHR = "2456e1b9-26e2-8f83-e744-f34f01e9d703";

// Known service UUIDs for the two BLE protocol families. Used by the
// auto-detect path below to recover gracefully when the user's
// configured charger model doesn't match what the charger actually
// speaks — typical case: Pulsar MAX on firmware ≥ 6.11.26, which
// Wallbox migrated to the dual-char protocol that previously only
// shipped on Plus / Copper / Quasar. mvanlijden issue #11.
static const char* MAX_SVC_UUID  = "2456e1b9-26e2-8f83-e744-f34f01e9d701";
static const char* MAX_CHR_UUID  = "2456e1b9-26e2-8f83-e744-f34f01e9d703";
static const char* PLUS_SVC_UUID = "331a36f5-2459-45ea-9d95-6142f0c4b307";
static const char* PLUS_CHR_UUID = "a9da6040-0823-4995-94ec-9ce41ca28833";
static const char* PLUS_TXC_UUID = "a73e9a10-628f-4494-a099-12efaf72258f";

class WBClientCallbacks : public NimBLEClientCallbacks {
    uint32_t onPassKeyRequest() override {
        uint32_t pk = wallboxBLE.blePasskey();
        Log.println("[BLE] SMP passkey request — sending configured PIN");
        return pk;
    }
    bool onConfirmPIN(uint32_t pin) override {
        Log.println("[BLE] SMP PIN confirmed");
        return true;
    }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        Log.printf("[BLE] SMP auth complete: encrypted=%d bonded=%d\n",
            desc->sec_state.encrypted, desc->sec_state.bonded);
    }
};
static WBClientCallbacks _secCallbacks;

void WallboxBLE::begin(const char* addr) {
    _instance = this;
    _addr = addr;
    _pin = "";
    _svcUUID = DEF_SVC;
    _chrUUID = DEF_CHR;
    _state = State::DISCONNECTED;
    Log.printf("[BLE] Target: %s\n", addr);

    // Phase 1 refactor: spin up a dedicated FreeRTOS task that owns the
    // BLE state machine. The Arduino main loop no longer calls loop()
    // directly — so BLE scans / connects / waits never freeze the web UI.
    // The mutex serialises _sendCommandDirect() callers (notably: the BLE task's
    // own keepalive, and the main task's poll cycles).
    if (!_cmdMutex) _cmdMutex = xSemaphoreCreateMutex();
    if (!_cacheMutex) _cacheMutex = xSemaphoreCreateMutex();
    // 2.7.0 step 1: BLE request queue infrastructure. Allocated here
    // alongside the existing mutexes; no callers yet — this is the
    // null-op scaffolding step. See docs/plans/2.7.0-api-command-async.md.
    if (!_reqQueue) _reqQueue = xQueueCreate(kBleReqQueueDepth, sizeof(BleReq));
    if (!_responseMapMutex) _responseMapMutex = xSemaphoreCreateMutex();
    if (!_pendingPubMutex)  _pendingPubMutex  = xSemaphoreCreateMutex();
    if (!_taskHandle) {
        xTaskCreatePinnedToCore(
            _taskFn, "wb_ble",
            8192,           // stack size
            this,
            1,              // priority — lower than NimBLE host (which is 5)
            &_taskHandle,
            1               // Core 1 (same as Arduino loop — Core 0 is NimBLE's)
        );
        Log.println("[BLE] State-machine task started on core 1");
    }
}

void WallboxBLE::_taskFn(void* arg) {
    WallboxBLE* self = (WallboxBLE*)arg;
    for (;;) {
        self->loop();
        vTaskDelay(pdMS_TO_TICKS(20));  // ~50 Hz tick — plenty for our pace
    }
}

void WallboxBLE::pause(uint32_t ms) {
    // CRITICAL: caller may be on the web/main task (e.g. OTA upload start);
    // _disconnect() tears down NimBLE objects that the BLE task on Core 1
    // owns and may be reading mid-frame. Calling it from here is a
    // use-after-free race that reliably panics the gateway under OTA
    // upload load (root cause of peter-mcc's "OTA still reboots" report
    // and reproduced locally on rc20 via curl multipart upload).
    //
    // Fix: only set the pause flag here. The BLE task's loop() observes
    // it on its next iteration and disconnects cleanly from its own
    // task context. Callers that need the BLE to be idle before
    // proceeding can poll isPaused()/stateStr() — though for OTA the
    // ~100ms it takes the BLE task to notice is well within the upload
    // window.
    _pausedUntil = millis() + ms;
    Log.printf("[BLE] pause requested for %u ms (BLE task will disconnect on next tick)\n", ms);
}

void WallboxBLE::loop() {
    if (isPaused()) {
        // Tear down BLE from THIS task's context (not the caller's) so we
        // never race a sendCommand / notification callback. Idempotent —
        // if we're already disconnected, _disconnect() is a no-op.
        if (_state == State::CONNECTED) {
            Log.println("[BLE] pause active — disconnecting from BLE task");
            _disconnect();
            _state = State::DISCONNECTED;
        }
        return;  // skip all further BLE activity during user-requested pause
    }
    switch (_state) {
    case State::DISCONNECTED:
    case State::ERROR:
        if (millis() - _lastConnectAttempt >= _connectBackoff) {
            _connect();
        }
        break;

    case State::CONNECTED:
        // Check connection is still alive
        if (!_client || !_client->isConnected()) {
            Log.println("[BLE] Connection lost");
            _state = State::DISCONNECTED;
            _chr = nullptr;
            break;
        }

        // Sample link RSSI on a fixed cadence and smooth with an EMA.
        // Single source of truth — every consumer reads the same value.
        if (millis() - _rssiLastSample >= RSSI_SAMPLE_MS) {
            _rssiLastSample = millis();
            int raw = _client->getRssi();
            if (raw > -110 && raw < 0) {
                if (_rssiSmoothed == -127) {
                    _rssiSmoothed = raw;
                } else {
                    // α≈0.15 — slow enough to absorb the wild per-packet
                    // swings NimBLE returns, fast enough to track real movement.
                    _rssiSmoothed = (3 * raw + 17 * _rssiSmoothed) / 20;
                }
            }
        }

        // Keepalive if idle (no commands sent recently)
        // Plus doesn't implement `ping` — use r_dat (status) which is universal
        if (millis() - _lastActivityTime >= PING_INTERVAL_MS) {
            const char* keepalive = isPlus() ? bapi::MET_GET_STATUS : bapi::MET_PING;
            String resp = _sendCommandDirect(keepalive, "null", 2000);
            if (resp.isEmpty()) {
                Log.printf("[BLE] Keepalive %s timeout — reconnecting\n", keepalive);
                _disconnect();
                break;
            }
        }

        // Process pending command
        if (_hasPending) {
            _hasPending = false;
            String framed = bapi::frame(_pendingCmd);
            if (_chr) {
                _parser.reset();
                _responseReady = false;
                _chr->writeValue((const uint8_t*)framed.c_str(), framed.length(), false);
                _txCount++;
                _lastActivityTime = millis();
            }
        }

        // ---- Phase 3 (2.7.0 steps 2+3): drain async request queue ----
        // Each enqueued BleReq is dispatched through sendCommand here
        // (safe — the BLE task already calls sendCommand for its own
        // keepalive, so re-entering doesn't deadlock _cmdMutex). At
        // most ONE request per loop iteration so periodic polling
        // doesn't get starved. Step 3: response stored in RAM map
        // keyed by req.reqId for tryFetchResponse() to pick up.
        // xTaskNotify wake-waiter path lands in step 4; MQTT-publish
        // back-channel in step 5.
        if (_reqQueue) {
            BleReq req;
            if (xQueueReceive(_reqQueue, &req, 0) == pdTRUE) {
                // Step 9d: honour per-request BAPI timeout if set.
                // Web handler passes ?wait through so the BAPI roundtrip
                // matches the HTTP wait — otherwise slow methods like
                // gupdc (10 s cloud check) gave up at the 5 s default
                // and the waiter saw a 202 even though the user asked
                // to wait 12 s. 0 = use the function's own default.
                // 5s default. Tried 15s to capture slow s_sch responses
                // but it caused panics — holding the BLE _cmdMutex for
                // 15s starved enough other paths to trigger a watchdog
                // panic. If a method genuinely needs a longer wait it
                // can override via bapiTimeoutMs from the caller.
                uint32_t to = req.bapiTimeoutMs ? req.bapiTimeoutMs : 5000;
                String resp = _sendCommandDirect(req.met, req.par, to);
                _storeResponse(req.reqId, resp);
                bool doWake = (req.replyMode == ReplyMode::WAKE_WAITER
                            || req.replyMode == ReplyMode::WAKE_AND_MQTT);
                bool doMqtt = (req.replyMode == ReplyMode::MQTT_PUBLISH
                            || req.replyMode == ReplyMode::WAKE_AND_MQTT);
                if (doWake && req.waiter) {
                    xTaskNotify(req.waiter, req.reqId, eSetValueWithOverwrite);
                }
                if (doMqtt && resp.length()) {
                    _enqueueMqttPub(String(req.met), resp);
                }
            }
        }

        // ---- Phase 2 (rc16): periodic polling, driven from this task ----
        // These were on the Arduino main loop before — moving them here
        // means the main loop never blocks waiting for BAPI responses.
        // Main task reads cached results via copyCachedXxx() and publishes.
        {
            uint32_t now = millis();
            if (now - _lastStatusPoll >= _statusPollMs) {
                _lastStatusPoll = now;
                _pollStatus();
            }
            if (now - _lastRealtimePoll >= _realtimePollMs) {
                _lastRealtimePoll = now;
                _pollRealtime();
                _pollSettings();   // 5-BAPI chain — but on BLE task, invisible to main
            }
            if (now - _lastNotifPoll >= NOTIF_POLL_MS) {
                _lastNotifPoll = now;
                _pollNotifications();
            }
        }
        break;

    case State::CONNECTING:
    case State::AUTHENTICATING:
        break;
    }
}

void WallboxBLE::_connect() {
    // Don't burn radio cycles scanning if no charger address is configured
    // (e.g., between web-UI save and reboot — config changed but begin() not re-called)
    if (_addr.length() == 0) {
        _state = State::DISCONNECTED;
        _connectBackoff = 5000;
        _lastConnectAttempt = millis();
        return;
    }

    _lastConnectAttempt = millis();
    _state = State::CONNECTING;

    String addrLower = _addr;
    addrLower.toLowerCase();

    // Check scan cache — skip scan if charger was seen recently
    bool skipScan = (_lastSeenTime > 0) && (millis() - _lastSeenTime < SCAN_CACHE_MS);

    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    if (!skipScan) {
        // Full scan
        Log.printf("[BLE] Scanning for %s...\n", addrLower.c_str());
        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);
        NimBLEScanResults results = scan->start(5, false);

        bool found = false;
        _scanRSSI = -127;
        _foundAddr = NimBLEAddress();
        for (int i = 0; i < results.getCount(); i++) {
            NimBLEAdvertisedDevice dev = results.getDevice(i);
            String devAddr = dev.getAddress().toString().c_str();
            devAddr.toLowerCase();
            if (devAddr == addrLower) {
                found = true;
                _scanRSSI = dev.getRSSI();
                _foundAddr = dev.getAddress();  // preserves correct address type
                _lastSeenTime = millis();
                String name = dev.getName().c_str();
                Log.printf("[BLE] Found! RSSI: %d dBm  Name: %s  Type: %d\n",
                    _scanRSSI, name.c_str(), dev.getAddress().getType());
                break;
            }
        }

        if (!found) {
            Log.printf("[BLE] Not found in scan (%d devices seen)\n", results.getCount());
            for (int i = 0; i < results.getCount() && i < 5; i++) {
                NimBLEAdvertisedDevice dev = results.getDevice(i);
                String name = dev.getName().c_str();
                if (name.length() > 0) {
                    Log.printf("[BLE]   %s %s RSSI:%d\n",
                        dev.getAddress().toString().c_str(), name.c_str(), dev.getRSSI());
                }
            }
            _state = State::ERROR;
            _connectBackoff = min(_connectBackoff * 2, (uint32_t)30000);
            esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
            return;
        }
    } else {
        Log.printf("[BLE] Seen %lus ago, connecting directly...\n",
            (millis() - _lastSeenTime) / 1000);
    }

    // Connect
    Log.printf("[BLE] Connecting (RSSI: %d)...\n", _scanRSSI);

    if (!_client) {
        _client = NimBLEDevice::createClient();
        _client->setConnectTimeout(10);
        _client->setClientCallbacks(&_secCallbacks, false);
    }

    // Use the address from scan (preserves correct type) or try both
    bool connected = false;
    if (_foundAddr.toString().length() > 0) {
        Log.printf("[BLE] Using scan address (type %d)...\n", _foundAddr.getType());
        connected = _client->connect(_foundAddr);
    }
    if (!connected) {
        Log.println("[BLE] Trying public address...");
        connected = _client->connect(NimBLEAddress(addrLower.c_str(), BLE_ADDR_PUBLIC));
    }
    if (!connected) {
        Log.println("[BLE] Trying random address...");
        connected = _client->connect(NimBLEAddress(addrLower.c_str(), BLE_ADDR_RANDOM));
    }

    if (!connected) {
        Log.printf("[BLE] Connection failed (RSSI %d)\n", _scanRSSI);
        _state = State::ERROR;
        _connectBackoff = min(_connectBackoff * 2, (uint32_t)30000);
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        return;
    }

    Log.println("[BLE] Connected, stabilizing...");
    delay(300);  // Let connection stabilize before anything

    // WiFi coexistence: widen BLE intervals so WiFi gets airtime
    _client->updateConnParams(30, 50, 2, 300);
    delay(200);

    // Enumerate the entire GATT tree once per connect — helps diagnose
    // future UUID rotations or new firmware exposing new endpoints.
    // Cheap (one-shot at connect, output is short).
    {
        std::vector<NimBLERemoteService*>* svcs = _client->getServices(true);
        if (svcs) {
            Log.printf("[BLE] GATT topology: %u service(s)\n", (unsigned)svcs->size());
            for (auto* s : *svcs) {
                Log.printf("[BLE]   svc %s\n", s->getUUID().toString().c_str());
                std::vector<NimBLERemoteCharacteristic*>* chars = s->getCharacteristics(true);
                if (!chars) continue;
                for (auto* c : *chars) {
                    String props;
                    if (c->canRead())     props += "R";
                    if (c->canWrite())    props += "W";
                    if (c->canWriteNoResponse()) props += "w";
                    if (c->canNotify())   props += "N";
                    if (c->canIndicate()) props += "I";
                    Log.printf("[BLE]     chr %s  [%s]\n",
                        c->getUUID().toString().c_str(),
                        props.length() ? props.c_str() : "-");
                }
            }
        }
    }

    // Discover services
    NimBLERemoteService* svc = _client->getService(_svcUUID.c_str());
    if (!svc) {
        // Auto-detect protocol-family mismatch. Typical trigger:
        // user configured Pulsar MAX, but their charger is on FW
        // ≥ 6.11.26 where Wallbox migrated MAX to the dual-char
        // protocol that previously only shipped on Plus/Copper/Quasar
        // (mvanlijden #11). Detect by probing for the *other*
        // family's service UUID — if present, switch in memory AND
        // persist to NVS so subsequent boots come up clean without
        // the user having to manually fix /config.
        bool weAreMax = _svcUUID.equalsIgnoreCase(MAX_SVC_UUID);
        bool weArePlus = _svcUUID.equalsIgnoreCase(PLUS_SVC_UUID);
        const char* otherSvcUUID = weAreMax ? PLUS_SVC_UUID
                                  : weArePlus ? MAX_SVC_UUID
                                  : nullptr;
        NimBLERemoteService* otherSvc = otherSvcUUID
            ? _client->getService(otherSvcUUID) : nullptr;
        if (otherSvc) {
            Log.println("[BLE] ===========================================================");
            Log.printf( "[BLE] Auto-switching protocol family: configured %s,\n",
                weAreMax ? "MAX (single-char)" : "Plus (dual-char)");
            Log.printf( "[BLE] but charger speaks %s — adopting it and saving config.\n",
                weAreMax ? "Plus (dual-char)" : "MAX (single-char)");
            Log.println("[BLE] (Wallbox migrated MAX to the dual-char protocol at FW 6.11.26.)");
            Log.println("[BLE] ===========================================================");
            WBConfig& cfg = configMgr.mut();
            if (weAreMax) {
                cfg.chargerModel = "plus";
                cfg.bleService   = PLUS_SVC_UUID;
                cfg.bleChar      = PLUS_CHR_UUID;
                cfg.bleTxChar    = PLUS_TXC_UUID;
                _svcUUID  = PLUS_SVC_UUID;
                _chrUUID  = PLUS_CHR_UUID;
                _txChrUUID = PLUS_TXC_UUID;
                _chargerModel = "plus";
            } else {
                cfg.chargerModel = "max";
                cfg.bleService   = MAX_SVC_UUID;
                cfg.bleChar      = MAX_CHR_UUID;
                cfg.bleTxChar    = "";
                _svcUUID  = MAX_SVC_UUID;
                _chrUUID  = MAX_CHR_UUID;
                _txChrUUID = "";
                _chargerModel = "max";
            }
            configMgr.save();
            // Resolve the now-correct service and fall through to the
            // characteristic lookup. The connection is still up, no
            // disconnect needed.
            svc = otherSvc;
        } else {
            Log.printf("[BLE] Service %s not found\n", _svcUUID.c_str());
            _client->disconnect();
            _state = State::ERROR;
            esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
            return;
        }
    }

    _chr = svc->getCharacteristic(_chrUUID.c_str());
    if (!_chr) {
        Log.println("[BLE] Characteristic not found");
        _client->disconnect();
        _state = State::ERROR;
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        return;
    }

    // Resolve notify-target characteristic:
    //   single-char mode (MAX): same characteristic as write (_chr)
    //   dual-char mode (Plus):  separate notify characteristic (_txChrUUID)
    NimBLERemoteCharacteristic* notifyChr = _chr;
    if (_txChrUUID.length() > 0) {
        notifyChr = svc->getCharacteristic(_txChrUUID.c_str());
        if (!notifyChr) {
            Log.printf("[BLE] TX/notify char %s not found\n", _txChrUUID.c_str());
            _client->disconnect();
            _state = State::ERROR;
            esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
            return;
        }
        Log.println("[BLE] Using dual-char mode (separate notify characteristic)");
    }

    // 3.0 schedule-write fix attempt: proactively pair/encrypt
    // before subscribing for notifies. jagheterfredrik's HACS
    // reference always pairs on connect, and the live Pulsar MAX
    // (fw 6.11.16) silently accepts reads from unpaired clients
    // but appears to reject settings writes (w_sch/s_sch/s_alo
    // all return "Unexpected Error" without it). Most chargers
    // without a BAPI PIN use "Just Works" pairing — no user
    // prompt. We do this BEFORE CCCD subscription rather than as
    // a fallback so the first-write authorisation is in place
    // from the start of the session.
    Log.println("[BLE] Initiating SMP encryption (proactive pair)...");
    bool secureOk = _client->secureConnection();
    if (secureOk) {
        Log.println("[BLE] Encryption established");
        // NimBLE 1.4.1 can return before bond info fully lands —
        // small delay matches the fallback path's behaviour.
        delay(200);
    } else {
        Log.printf("[BLE] secureConnection() failed (err 0x%02x) — "
                   "continuing unpaired; writes may be rejected\n",
                   _client->getLastError());
    }

    // Subscribe to notifications — if CCCD write is rejected, encrypt and retry
    bool notifyOk = notifyChr->canNotify() && notifyChr->registerForNotify(_notifyCb);
    if (!notifyOk && notifyChr->canNotify()) {
        Log.println("[BLE] CCCD rejected, trying SMP encryption...");
        delay(200);
        if (_client->secureConnection()) {
            Log.println("[BLE] Encrypted, retrying notifications...");
            // NimBLE 1.4.1 can return from secureConnection() before bond
            // info fully lands — give it a moment before the CCCD retry
            delay(200);
            notifyOk = notifyChr->registerForNotify(_notifyCb);
        } else {
            Log.printf("[BLE] Encryption failed (err 0x%02x)\n", _client->getLastError());
        }
    }
    if (!notifyOk) {
        Log.println("[BLE] Failed to register for notifications");
        _client->disconnect();
        _state = State::ERROR;
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        return;
    }

    Log.println("[BLE] Notifications enabled");

    // BGX13P / BGXSS stream-mode switch.
    // Pulsar Plus (and Copper SB / Quasar) use a Silicon Labs BGX13P module
    // as a Bluetooth-to-UART bridge to the Wallbox firmware. The BGX has a
    // "Mode" characteristic (UUID 75a9f022-af03-4e41-b4bc-9de90a47d50b) that
    // selects between STREAM_MODE (1, data passthrough) and REMOTE_COMMAND_
    // MODE (3, our writes interpreted as BGX commands and dropped before
    // reaching Wallbox). Default depends on BGX firmware config — some
    // boards boot STREAM, others REMOTE_COMMAND. Force STREAM_MODE so our
    // BAPI bytes always reach the charger MCU.
    //
    // Plus-family only — MAX uses u-blox single-char and doesn't have this.
    if (isPlus()) {
        static const char* BGX_MODE_UUID = "75a9f022-af03-4e41-b4bc-9de90a47d50b";
        NimBLERemoteCharacteristic* modeChr = svc->getCharacteristic(BGX_MODE_UUID);
        if (modeChr) {
            std::string current = modeChr->readValue();
            uint8_t curMode = current.length() > 0 ? (uint8_t)current[0] : 0xff;
            Log.printf("[BLE] BGX mode char found, current value: 0x%02x\n", curMode);
            if (curMode != 0x01) {  // 1 = STREAM_MODE per Silicon Labs BGXpress
                uint8_t streamMode = 0x01;
                if (modeChr->writeValue(&streamMode, 1, true)) {
                    Log.println("[BLE] BGX switched to STREAM_MODE — BAPI passthrough enabled");
                    delay(200);  // give the BGX a beat to honour the mode change
                } else {
                    Log.println("[BLE] BGX mode write failed — BAPI may not pass through");
                }
            } else {
                Log.println("[BLE] BGX already in STREAM_MODE");
            }
        } else {
            Log.println("[BLE] BGX mode char not found — assuming non-BGX peripheral");
        }
    }

    // Read GATT Device Info service (0x180A) if present
    NimBLERemoteService* devInfo = _client->getService("180a");
    if (devInfo) {
        NimBLERemoteCharacteristic* c;
        if ((c = devInfo->getCharacteristic("2a29")) != nullptr) _devMfg = c->readValue().c_str();
        if ((c = devInfo->getCharacteristic("2a24")) != nullptr) _devModel = c->readValue().c_str();
        if ((c = devInfo->getCharacteristic("2a26")) != nullptr) _devFw = c->readValue().c_str();
        Log.printf("[BLE] Device: %s / %s / FW %s\n",
            _devMfg.c_str(), _devModel.c_str(), _devFw.c_str());

        // Detect Wallbox auto-OTAs by comparing against the persisted baseline.
        // First boot (saved == "") seeds silently — no banner for the initial sighting.
        if (_devFw.length() > 0) {
            const String& saved = configMgr.get().lastSeenFw;
            if (saved.length() > 0 && saved != _devFw) {
                Log.printf("[BLE] Charger FW CHANGED: %s -> %s\n", saved.c_str(), _devFw.c_str());
                _prevFw = saved;
                _fwChanged = true;
            }
            if (saved != _devFw) {
                configMgr.mut().lastSeenFw = _devFw;
                configMgr.save();
            }
        }
    }
    // Also read generic Device Name (0x1800 / 0x2a00)
    NimBLERemoteService* ga = _client->getService("1800");
    if (ga) {
        NimBLERemoteCharacteristic* nc = ga->getCharacteristic("2a00");
        if (nc) _devName = nc->readValue().c_str();
    }

    _connectBackoff = 2000;  // Fast reconnect after success
    _lastSeenTime = millis();  // Refresh scan cache
    _lastActivityTime = millis();

    // Restore balanced coexistence
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

    // Authenticate if needed (Plus skips read_pin; MAX does the PIN dance)
    if (_authenticate()) {
        _state = State::CONNECTED;

        // Read charger identity once per connect — works on MAX + Plus.
        // r_sn_ returns the serial; g_mac returns the charger's MAC addresses.
        // Best-effort: failures just leave the fields empty.
        String snResp = _sendCommandDirect(bapi::MET_GET_SERIAL, "null", 2000);
        if (!snResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, snResp) == DeserializationError::Ok) {
                if (d["r"].is<const char*>()) _chgSerial = d["r"].as<const char*>();
                else if (d["r"]["sn"].is<const char*>()) _chgSerial = d["r"]["sn"].as<const char*>();
            }
        }
        String macResp = _sendCommandDirect(bapi::MET_GET_MAC, "null", 2000);
        if (!macResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, macResp) == DeserializationError::Ok) {
                // Spec varies across firmware. MAX returns a FLAT object:
                //   {"eth_mac":"...","wlan_mac":"...","id":N}
                // Plus/Copper likely wrap under "r". Try both shapes;
                // prefer wlan_mac (BLE radio shares the WiFi chip).
                auto pickMac = [](JsonVariantConst obj) -> const char* {
                    if (obj["wlan_mac"].is<const char*>()) return obj["wlan_mac"].as<const char*>();
                    if (obj["wifi"].is<const char*>())     return obj["wifi"].as<const char*>();
                    if (obj["mac"].is<const char*>())      return obj["mac"].as<const char*>();
                    if (obj["eth_mac"].is<const char*>())  return obj["eth_mac"].as<const char*>();
                    return nullptr;
                };
                const char* m = pickMac(d["r"]);
                if (!m || strlen(m) == 0 || strcmp(m, "0") == 0) m = pickMac(d.as<JsonVariantConst>());
                if (m && strlen(m) > 0 && strcmp(m, "0") != 0) _chgMac = m;
            }
        }
        if (_chgSerial.length() || _chgMac.length()) {
            Log.printf("[BLE] Charger SN: %s  MAC: %s\n",
                _chgSerial.length() ? _chgSerial.c_str() : "(none)",
                _chgMac.length() ? _chgMac.c_str() : "(none)");
        }

        // Grounding status (r_wel) — universal safety diagnostic. The
        // response format varies (some firmware returns plain "ok"/"fault",
        // others return a JSON object {"r":{...}}) — store whatever we get
        // so the UI can do best-effort display.
        String welResp = _sendCommandDirect(bapi::MET_GET_GROUNDING, "null", 2000);
        if (!welResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, welResp) == DeserializationError::Ok) {
                if (d["r"].is<const char*>()) {
                    _chgGrounding = d["r"].as<const char*>();
                } else if (d["r"]["status"].is<const char*>()) {
                    _chgGrounding = d["r"]["status"].as<const char*>();
                } else if (d["r"]["grounding"].is<const char*>()) {
                    _chgGrounding = d["r"]["grounding"].as<const char*>();
                } else if (d["r"].is<int>()) {
                    // Integer status — render as "Code N" rather than
                    // guessing OK/Fault. Different charger families use
                    // different "healthy" codes: MAX returns 0 when
                    // healthy, Plus returns 1 when healthy (verified on
                    // peter-mcc's BGX13P unit in issue #4 — charger
                    // operating normally, Wallbox app shows no
                    // problems, so 1 must be Plus's nominal value, not
                    // a fault). The earlier "non-zero = Fault" mapping
                    // was MAX-specific and false-alarmed Plus users.
                    //
                    // 0 is the one value we've observed AND know to be
                    // healthy (on MAX) — display as "OK" for that one.
                    // Everything else: raw code. If anyone ever reports
                    // an actual contactor-weld fault with a specific
                    // integer we'll map that explicitly. Until then,
                    // neutral framing avoids both false alarms and
                    // hidden real ones.
                    int code = d["r"].as<int>();
                    _chgGrounding = (code == 0) ? String("OK") : String("Code ") + code;
                }
            }
            if (_chgGrounding.length()) {
                Log.printf("[BLE] Grounding: %s\n", _chgGrounding.c_str());
            }
        }

        // Charger application firmware + project (fw_v_) — the version
        // Wallbox app shows the user, plus the project name we use as
        // canonical model identification. Both are stable across the
        // session — read once at init. Method name documented by
        // jagheterfredrik/wallbox-ble (GET_CHARGER_VERSIONS = "fw_v_").
        // Response shape (confirmed on MAX, expected to match on Plus):
        //   { id, c, db, fw, p, r, s }
        // where s = human-readable version, p = project ("prj15-pulsar-max").
        String fwvResp = _sendCommandDirect(bapi::MET_GET_CHARGER_VER, "null", 2000);
        if (!fwvResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, fwvResp) == DeserializationError::Ok) {
                if (d["s"].is<const char*>()) _chgAppFw = d["s"].as<const char*>();
                if (d["p"].is<const char*>()) _chgProject = d["p"].as<const char*>();
            }
            if (_chgAppFw.length() || _chgProject.length()) {
                Log.printf("[BLE] Charger FW: %s  project: %s\n",
                    _chgAppFw.length() ? _chgAppFw.c_str() : "(none)",
                    _chgProject.length() ? _chgProject.c_str() : "(none)");
            }
            // Tell the main loop to re-publish HA discovery so the
            // device sw_version updates from WB_VERSION fallback to
            // the real charger app FW. Only useful if we actually got
            // something — no point re-publishing the fallback as the
            // fallback.
            if (_chgAppFw.length()) _discoveryStale = true;
        }

        // Total session count from r_ses. peter-mcc 2.4.1 follow-up:
        // 2.4.1 used `size` which is actually the log buffer CAPACITY
        // sentinel (99999 on MAX, missing on Plus) — not lifetime count.
        // The real count is in `last`, confirmed via raw BAPI probe on
        // MAX: {"last":233,"size":99999} — 233 is plausible, 99999 is
        // the storage-cap marker.
        //
        // Defensive parse: prefer `last`. Fall back to `size` only if
        // it's in a plausible range (< 50000) — anything in the 99999
        // ballpark is the sentinel and worse than no data at all. If
        // neither yields a real number, leave _chgSessionCount at -1
        // and the publish path emits null so HA marks the sensor
        // unavailable instead of showing -1.
        String sesResp = _sendCommandDirect(bapi::MET_GET_SESSIONS, "null", 2000);
        if (!sesResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, sesResp) == DeserializationError::Ok) {
                if (d["r"]["last"].is<int>()) {
                    _chgSessionCount = d["r"]["last"].as<int32_t>();
                } else if (d["r"]["size"].is<int>()) {
                    int s = d["r"]["size"].as<int>();
                    if (s >= 0 && s < 50000) _chgSessionCount = s;
                }
                if (_chgSessionCount >= 0) {
                    Log.printf("[BLE] Total sessions: %d\n", (int)_chgSessionCount);
                } else {
                    Log.println("[BLE] Total sessions: not exposed by this charger");
                }
            }
        }

        // Power Boost limit (r_hsh) — household-meter-tied current cap.
        // Per jagheterfredrik/wallbox-ble: GET_POWER_BOOST = "r_hsh".
        // Observed return on MAX: plain integer (e.g. 63). Refreshed
        // each (re)connect — config changes are rare.
        String hshResp = _sendCommandDirect(bapi::MET_GET_POWER_BOOST, "null", 2000);
        if (!hshResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, hshResp) == DeserializationError::Ok && d["r"].is<int>()) {
                _chgPowerBoost = d["r"].as<int32_t>();
                Log.printf("[BLE] Power Boost: %d\n", (int)_chgPowerBoost);
            }
        }

        // Discrete lock state (r_lck). Same fact as r_sta.lock_status
        // but as its own field, which lets HA wire a dedicated lock
        // entity rather than parsing the realtime blob.
        String lckResp = _sendCommandDirect(bapi::MET_GET_LOCK_STATE, "null", 2000);
        if (!lckResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, lckResp) == DeserializationError::Ok && d["r"].is<int>()) {
                _chgLockState = d["r"].as<int32_t>();
                Log.printf("[BLE] Lock state: %d\n", (int)_chgLockState);
            }
        }

        // Charger-side network status (gnsta). Returns an array of
        // active network configurations — we pull the first one for
        // SSID/IP/RSSI. The charger has its OWN WiFi link (separate
        // from our gateway's link to the user's WiFi), so this is
        // distinct diagnostic information.
        String nstaResp = _sendCommandDirect(bapi::MET_GET_NETWORKS, "null", 2000);
        if (!nstaResp.isEmpty()) {
            JsonDocument d;
            if (deserializeJson(d, nstaResp) == DeserializationError::Ok) {
                JsonVariant first;
                if (d["r"].is<JsonArray>() && d["r"].size() > 0) first = d["r"][0];
                else if (d["r"].is<JsonObject>()) first = d["r"];
                if (!first.isNull()) {
                    if (first["ssid"].is<const char*>())   _chgNetSsid = first["ssid"].as<const char*>();
                    if (first["ip"].is<const char*>())     _chgNetIp   = first["ip"].as<const char*>();
                    // Pulsar MAX returns a `signal` quality percent (0-100).
                    // No `rssi` field observed in the live BAPI probe. We
                    // accept either with `signal` winning if both appear.
                    if (first["signal"].is<int>()) _chgNetSignal = first["signal"].as<int>();
                    else if (first["rssi"].is<int>()) {
                        int r = first["rssi"].as<int>();
                        // Convert RSSI in dBm to a rough quality % so HA's
                        // single sensor renders consistently across firmware
                        // variants. -50 dBm -> ~100 %, -100 dBm -> ~0 %.
                        if (r < 0) {
                            int q = 2 * (r + 100);
                            if (q < 0) q = 0; if (q > 100) q = 100;
                            _chgNetSignal = q;
                        } else _chgNetSignal = r;
                    }
                }
            }
            if (_chgNetIp.length()) {
                Log.printf("[BLE] Charger network: ssid=%s ip=%s rssi=%d\n",
                    _chgNetSsid.length() ? _chgNetSsid.c_str() : "(none)",
                    _chgNetIp.c_str(), _chgNetSignal);
            }
        }

        Log.println("[BLE] Ready");
    }
}

void WallboxBLE::_disconnect() {
    if (_client && _client->isConnected()) {
        _client->disconnect();
    }
    _chr = nullptr;
    _state = State::DISCONNECTED;
    _rssiSmoothed = -127;  // reset EMA so the next connect starts fresh
    _rssiLastSample = 0;
    _seenBapiThisConnection = false;  // re-enable raw-RX logging for next connect
}

bool WallboxBLE::_authenticate() {
    _state = State::AUTHENTICATING;

    // Probe BAPI PIN status. read_pin is the original MAX auth method;
    // historically Plus had no equivalent (jagheterfredrik/wallbox-ble) but
    // Wallbox added a Bluetooth PIN system-wide at firmware 6.11+, so newer
    // Plus firmware may also implement it. Use a short timeout on Plus since
    // older firmware just silently drops the request.
    Log.println("[BLE] Checking PIN status...");
    uint32_t pinTimeout = isPlus() ? 2000 : 5000;
    String pinResp = _sendCommandDirect(bapi::MET_READ_PIN, "null", pinTimeout);

    if (pinResp.isEmpty()) {
        // No read_pin support on this firmware. Confirm the BAPI channel
        // actually works (via r_dat) before declaring "no PIN needed" —
        // otherwise a dead link looks identical to an unauthed-but-open one.
        Log.println("[BLE] No read_pin response — probing r_dat to confirm channel...");
        String probe = _sendCommandDirect(bapi::MET_GET_STATUS, "null", 5000);
        if (probe.isEmpty()) {
            Log.println("[BLE] r_dat also silent — channel may be broken or auth-gated");
        } else {
            Log.printf("[BLE] r_dat OK (%d bytes) — no BAPI PIN on this firmware\n", probe.length());
        }
        _pinRequired = false;
        return true;
    }

    JsonDocument doc;
    if (deserializeJson(doc, pinResp) != DeserializationError::Ok) {
        _pinRequired = false;
        return true;
    }

    const char* pin = doc["r"]["pin"] | (const char*)nullptr;
    int version = doc["r"]["version"] | 0;

    if (!pin || strlen(pin) == 0) {
        Log.println("[BLE] No PIN set — open access");
        _pinRequired = false;
        return true;
    }

    Log.printf("[BLE] PIN is set (version=%d)\n", version);
    _pinRequired = true;

    if (_pin.isEmpty()) {
        Log.println("[BLE] WARNING: Charger has PIN but none configured!");
        return true;
    }

    Log.println("[BLE] Authenticating with PIN...");
    String par = "{\"pin\":\"" + _pin + "\",\"version\":" + String(version) + "}";

    String authResp = _sendCommandDirect(bapi::MET_SET_PIN, par.c_str(), 5000);
    if (!authResp.isEmpty()) {
        JsonDocument authDoc;
        if (deserializeJson(authDoc, authResp) == DeserializationError::Ok) {
            if (authDoc["error"].is<JsonObject>()) {
                Log.printf("[BLE] PIN auth failed: %s\n",
                    authDoc["error"]["message"].as<const char*>());
            } else {
                Log.println("[BLE] PIN authenticated");
            }
        }
    }
    return true;
}

// Static notification callback
void WallboxBLE::_notifyCb(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify) {
    if (!_instance) return;
    _instance->_rxCount++;

    // Diagnostic — log raw bytes when the frame doesn't start with the BAPI
    // magic ("EaE") AND we haven't already seen valid BAPI traffic on this
    // connection. The point of this log is catching BGX command-mode error
    // replies ("ERR\r\n", "ready\r\n" etc.) at connect time on Plus. Once
    // we've seen even one valid BAPI response, subsequent non-EaE packets
    // are just continuations of multi-packet frames — logging them all
    // floods the in-RAM log buffer (it wrapped in ~2 minutes on MAX,
    // making post-incident diagnosis useless). After the first valid
    // BAPI response, stay quiet.
    if (!_instance->_seenBapiThisConnection
        && len > 0
        && !(len >= 3 && data[0] == 'E' && data[1] == 'a' && data[2] == 'E')) {
        String hex, ascii;
        size_t show = len < 32 ? len : 32;
        for (size_t i = 0; i < show; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", data[i]);
            hex += buf;
            ascii += (data[i] >= 0x20 && data[i] < 0x7f) ? (char)data[i] : '.';
        }
        Log.printf("[BLE] RX raw (%u): %s|%s\n", (unsigned)len, hex.c_str(), ascii.c_str());
    }

    bool complete = _instance->_parser.feed(data, len);
    if (complete) {
        _instance->_lastResponse = _instance->_parser.json();
        _instance->_responseReady = true;
        _instance->_seenBapiThisConnection = true;
        if (_instance->_responseCb) {
            _instance->_responseCb(_instance->_lastResponse);
        }
    }
}

// 2.7.0 step 5 — defer an MQTT publish to the main task. Called
// from the BLE-task drain when a request's replyMode is MQTT_PUBLISH.
// Cap at kPendingPubSize entries; drops oldest with a log line if the
// ring fills (which only happens if main task drain is wedged —
// shouldn't happen in normal flow).
void WallboxBLE::_enqueueMqttPub(const String& met, const String& json) {
    if (met.length() == 0 || json.length() == 0) return;
    if (!_pendingPubMutex) return;
    if (xSemaphoreTake(_pendingPubMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint8_t next = (_pendingPubHead + 1) % kPendingPubSize;
    if (next == _pendingPubTail) {
        // Ring full — drop oldest. Tail advance frees a slot.
        Log.printf("[BLE] pending MQTT pub ring full, dropping met=%s\n",
                   _pendingPub[_pendingPubTail].met.c_str());
        _pendingPubTail = (_pendingPubTail + 1) % kPendingPubSize;
    }
    _pendingPub[_pendingPubHead].met  = met;
    _pendingPub[_pendingPubHead].json = json;
    _pendingPubHead = next;
    xSemaphoreGive(_pendingPubMutex);
}

// 2.7.0 step 5 — drain one pending MQTT publish. Called from main
// task loop. Returns false when the ring is empty so the caller can
// break out of the drain loop. The main task should call this in a
// loop until false to flush all pending pubs each iteration.
bool WallboxBLE::drainPendingResponsePub(String& out_met, String& out_json) {
    if (!_pendingPubMutex) return false;
    if (xSemaphoreTake(_pendingPubMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    bool got = false;
    if (_pendingPubTail != _pendingPubHead) {
        out_met  = _pendingPub[_pendingPubTail].met;
        out_json = _pendingPub[_pendingPubTail].json;
        // Free the heap-backed Strings before advancing the tail.
        _pendingPub[_pendingPubTail].met  = String();
        _pendingPub[_pendingPubTail].json = String();
        _pendingPubTail = (_pendingPubTail + 1) % kPendingPubSize;
        got = true;
    }
    xSemaphoreGive(_pendingPubMutex);
    return got;
}

// 2.7.0 step 3 — store a completed response in the RAM map. Called
// from the BLE task's drain loop after sendCommand returns. Mutex-
// protected; safe to read from any task via tryFetchResponse.
void WallboxBLE::_storeResponse(uint32_t reqId, const String& json) {
    if (reqId == 0 || json.length() == 0) return;
    if (!_responseMapMutex) return;
    if (xSemaphoreTake(_responseMapMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Log.printf("[BLE] _storeResponse %u — mutex timeout\n", (unsigned)reqId);
        return;
    }
    ResponseSlot& slot = _responseMap[_responseMapHead];
    if (slot.reqId != 0) {
        // Evicting an unread response. Log once so a chronic poll-
        // never-called bug shows up in /api/logs.
        Log.printf("[BLE] response map evicting reqId=%u (unread)\n",
                   (unsigned)slot.reqId);
    }
    slot.reqId       = reqId;
    slot.completedAt = millis();
    slot.json        = json;
    _responseMapHead = (_responseMapHead + 1) % kResponseMapSize;
    xSemaphoreGive(_responseMapMutex);
}

// 2.7.0 step 3 — fetch a completed response by request id. Returns
// true and fills `out` if found; false if not yet completed or
// already evicted.
bool WallboxBLE::tryFetchResponse(uint32_t reqId, String& out) {
    if (reqId == 0 || !_responseMapMutex) return false;
    if (xSemaphoreTake(_responseMapMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool found = false;
    for (uint8_t i = 0; i < kResponseMapSize; i++) {
        if (_responseMap[i].reqId == reqId) {
            out = _responseMap[i].json;
            // Consume — slot becomes available for next eviction.
            _responseMap[i].reqId = 0;
            _responseMap[i].json = String();  // free heap
            found = true;
            break;
        }
    }
    xSemaphoreGive(_responseMapMutex);
    return found;
}

// 2.7.0 step 2 — non-blocking enqueue of a BAPI request onto the BLE
// task's internal queue. Returns the assigned request id; 0 on failure
// (queue full or not initialised). The BLE task drains and dispatches
// via sendCommand internally; response goes to the RAM map (step 3)
// keyed by request id; waiter wake-up lands in step 4.
uint32_t WallboxBLE::enqueueRequest(const char* met, const char* par,
                                    ReplyMode replyMode, TaskHandle_t waiter,
                                    uint32_t bapiTimeoutMs) {
    if (!_reqQueue || !met) return 0;
    BleReq req = {};
    // Step 9 hardening: __atomic_fetch_add is lock-free and safe under
    // the documented concurrent callers (main task via web handler,
    // BLE task via MQTT callback during sendCommand yield). Loop to
    // skip the 0 sentinel — under wrap, two racing increments could
    // both land on 0, so we retry until we get nonzero.
    uint32_t id;
    do {
        id = __atomic_fetch_add(&_nextReqId, 1, __ATOMIC_RELAXED);
    } while (id == 0);
    req.reqId      = id;
    strncpy(req.met, met, sizeof(req.met) - 1);
    req.met[sizeof(req.met) - 1] = '\0';
    if (par) {
        strncpy(req.par, par, sizeof(req.par) - 1);
        req.par[sizeof(req.par) - 1] = '\0';
    }
    req.replyMode      = replyMode;
    req.waiter         = waiter;
    req.enqueuedAt     = millis();
    req.bapiTimeoutMs  = bapiTimeoutMs;
    if (xQueueSend(_reqQueue, &req, 0) != pdTRUE) {
        Log.printf("[BLE] enqueueRequest %s — queue full, dropped\n", met);
        return 0;
    }
    return req.reqId;
}

String WallboxBLE::_sendCommandDirect(const char* met, const char* par, uint32_t timeoutMs) {
    // Serialise BAPI commands across tasks. The BLE task's own keepalive
    // and the Arduino main task's polls can both end up here — without
    // this mutex, two writeValue() calls on _chr could race and corrupt
    // _parser/_responseReady state. Wait at most timeoutMs + 1s for the
    // mutex; if we can't get it, treat as a timeout (and don't bash _chr).
    if (_cmdMutex && xSemaphoreTake(_cmdMutex, pdMS_TO_TICKS(timeoutMs + 1000)) != pdTRUE) {
        Log.printf("[BLE] sendCommand %s — mutex busy, skipping\n", met);
        return "";
    }

    // Connection health check before sending
    if (!_chr || !_client || !_client->isConnected()) {
        if (_state == State::CONNECTED) {
            Log.printf("[BLE] Connection lost (detected before %s)\n", met);
            _state = State::DISCONNECTED;
            _chr = nullptr;
        }
        if (_cmdMutex) xSemaphoreGive(_cmdMutex);
        return "";
    }

    _lastActivityTime = millis();

    int id = _nextId++;
    String cmd = bapi::buildCmd(met, par, id);
    String framed = bapi::frame(cmd);

    _parser.reset();
    _responseReady = false;

    Log.printf("[BLE] TX %s\n", met);

    // Write — try without response first, fallback to with response
    if (!_chr->writeValue((const uint8_t*)framed.c_str(), framed.length(), false)) {
        if (!_chr->writeValue((const uint8_t*)framed.c_str(), framed.length(), true)) {
            Log.printf("[BLE] Write failed for %s — connection may be dead\n", met);
            _state = State::DISCONNECTED;
            _chr = nullptr;
            if (_cmdMutex) xSemaphoreGive(_cmdMutex);
            return "";
        }
    }
    _txCount++;

    // Wait for response — yield to other tasks (web server, OTA) while waiting
    uint32_t start = millis();
    while (!_responseReady && (millis() - start) < timeoutMs) {
        delay(1);
        if (_yieldCb) _yieldCb();  // let web server + OTA run
    }

    if (!_responseReady) {
        Log.printf("[BLE] Timeout %s (%dms)\n", met, (int)(millis() - start));
        // Check if connection died during wait
        if (_client && !_client->isConnected()) {
            Log.println("[BLE] Connection lost during command");
            _state = State::DISCONNECTED;
            _chr = nullptr;
        }
        if (_cmdMutex) xSemaphoreGive(_cmdMutex);
        return "";
    }

    Log.printf("[BLE] RX %s (%d bytes)\n", met, _lastResponse.length());
    String resp = _lastResponse;
    if (_cmdMutex) xSemaphoreGive(_cmdMutex);
    return resp;
}

// 2.7.0 step 4 — public sendCommand is now a thin wrapper around
// enqueueRequest + xTaskNotifyWait. External callers (web handlers,
// MQTT subscribe callbacks, anything off the BLE task) end up here
// — they enqueue and wait on a per-task notification. The BLE task
// itself never calls this (would deadlock its own queue); it uses
// _sendCommandDirect.
//
// Behavior matches the old sync sendCommand from the caller's POV:
// blocking, returns the response JSON or empty string on timeout.
// What's different under the hood: the actual BAPI roundtrip happens
// on the BLE task's drain loop (which calls _sendCommandDirect for
// the wire work). This means concurrent /api/command requests get
// serialised via the queue + drain rather than the older
// "_cmdMutex contention with the periodic poll".
String WallboxBLE::sendCommand(const char* met, const char* par, uint32_t timeoutMs) {
    if (!_reqQueue) {
        // Queue not initialised yet (called from setup before begin?).
        // Fall back to direct so we don't lose the call.
        return _sendCommandDirect(met, par, timeoutMs);
    }
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    // Defensive: if somehow we're called from the BLE task itself,
    // skip the queue and call direct. Same self-deadlock guard the
    // existing keepalive relied on by calling _sendCommandDirect
    // explicitly; this guard handles future callers that miss the
    // convention.
    if (self == _taskHandle) {
        return _sendCommandDirect(met, par, timeoutMs);
    }
    // Clear any stale notification value before enqueueing.
    xTaskNotifyStateClear(self);
    uint32_t reqId = enqueueRequest(met, par, ReplyMode::WAKE_WAITER, self);
    if (reqId == 0) {
        Log.printf("[BLE] sendCommand %s — queue full, returning empty\n", met);
        return "";
    }
    // Wait for the drain loop to notify us. Add ~250 ms headroom so
    // the notification window covers BLE write + response + storeResponse
    // overhead beyond the bare timeoutMs the request itself uses.
    uint32_t waitTicks = pdMS_TO_TICKS(timeoutMs + 250);
    uint32_t notified = 0;
    if (xTaskNotifyWait(0, ULONG_MAX, &notified, waitTicks) != pdTRUE) {
        Log.printf("[BLE] sendCommand %s — wait timeout (reqId=%u)\n",
                   met, (unsigned)reqId);
        return "";
    }
    if (notified != reqId) {
        // Stale notification from a previous request. Try the map
        // anyway in case our response is sitting there.
        Log.printf("[BLE] sendCommand %s — stale notify=%u, expected=%u\n",
                   met, (unsigned)notified, (unsigned)reqId);
    }
    String out;
    if (!tryFetchResponse(reqId, out)) {
        Log.printf("[BLE] sendCommand %s — notify received but map empty (evicted?)\n", met);
        return "";
    }
    return out;
}

void WallboxBLE::queueCommand(const char* met, const char* par) {
    int id = _nextId++;
    _pendingCmd = bapi::buildCmd(met, par, id);
    _hasPending = true;
}

const char* WallboxBLE::stateStr() const {
    switch (_state) {
    case State::DISCONNECTED:   return "disconnected";
    case State::CONNECTING:     return "connecting";
    case State::AUTHENTICATING: return "authenticating";
    case State::CONNECTED:      return "connected";
    case State::ERROR:          return "error";
    }
    return "unknown";
}

int WallboxBLE::rssi() const {
    // Pure getter — no HCI call. The smoothed value is updated only inside
    // loop() so all consumers (WS broadcast, /api/status, MQTT gateway-info)
    // see exactly the same number at any given moment.
    if (!_client || !_client->isConnected()) return _scanRSSI;
    return _rssiSmoothed == -127 ? _scanRSSI : _rssiSmoothed;
}

// ---------- Phase 2: periodic polling on the BLE task ----------

void WallboxBLE::_storeCache(String& dst, uint32_t& seq, const String& value) {
    if (!_cacheMutex) return;
    if (xSemaphoreTake(_cacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        dst = value;
        seq++;
        xSemaphoreGive(_cacheMutex);
    }
}

void WallboxBLE::_pollStatus() {
    if (_state != State::CONNECTED) return;
    String resp = _sendCommandDirect(bapi::MET_GET_STATUS);
    if (!resp.isEmpty()) _storeCache(_cachedStatusJson, _seqStatus, resp);
    // Energy meter on same cycle — lightweight & useful
    if (_state != State::CONNECTED) return;
    String meter = _sendCommandDirect(bapi::MET_GET_METER);
    if (!meter.isEmpty()) _storeCache(_cachedMeterJson, _seqMeter, meter);
}

void WallboxBLE::_pollRealtime() {
    if (_state != State::CONNECTED) return;
    String resp = _sendCommandDirect(bapi::MET_GET_REALTIME);
    if (!resp.isEmpty()) _storeCache(_cachedRealtimeJson, _seqRealtime, resp);
}

void WallboxBLE::_pollSettings() {
    // Five sequential BAPI reads, merged into one JSON. Each one uses a
    // 2-second timeout (vs the default 5s) so the mutex doesn't get held
    // for ~15s in the worst case — user-initiated BAPI commands (web,
    // MQTT) need to be able to slip in between these settings reads
    // without the user perceiving a hang. Settings reads on a healthy
    // link complete in well under 1s.
    static const uint32_t SETTINGS_TIMEOUT_MS = 2000;
    if (_state != State::CONNECTED) return;
    JsonDocument merged;

    String r1 = _sendCommandDirect(bapi::MET_GET_AUTOLOCK, "null", SETTINGS_TIMEOUT_MS);
    if (!r1.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r1) == DeserializationError::Ok) {
            // g_alo returns the lock timeout in seconds as a bare scalar:
            //   {"r": 0}   = auto-lock off
            //   {"r": 60}  = on, lock 60 s after disconnect
            // Older or hypothetical object form {r:{enabled, time}} is kept
            // as a defensive fallback. Confirmed bare-scalar on Pulsar MAX
            // (per benvanmierloo PR #9 + live probe). The HA timeout shows
            // minutes to match the Wallbox app — converted at the BAPI
            // boundary.
            int t = 0;
            if (d["r"].is<JsonObject>()) {
                t = d["r"]["time"] | 0;
                bool en = d["r"]["enabled"].as<bool>() || t > 0;
                merged["autolock"] = en ? 1 : 0;
            } else {
                t = d["r"].as<int>();
                merged["autolock"] = t > 0 ? 1 : 0;
            }
            if (t > 0) {
                int mins = (t + 30) / 60;  // round-to-nearest, clamp >=1
                if (mins < 1) mins = 1;
                _lastAutolockMin = mins;
            }
            merged["autolock_time"] = _lastAutolockMin;
        }
    }
    if (_state != State::CONNECTED) return;

    String r2 = _sendCommandDirect("g_ecos", "null", SETTINGS_TIMEOUT_MS);
    if (!r2.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r2) == DeserializationError::Ok) {
            merged["eco_mode"] = d["r"]["esm"] | 0;
            merged["eco_power"] = d["r"]["esp"] | 100;
        }
    }
    if (_state != State::CONNECTED) return;

    String r3 = _sendCommandDirect("g_psh", "null", SETTINGS_TIMEOUT_MS);
    if (!r3.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r3) == DeserializationError::Ok) {
            // dyps may arrive as bool true/false instead of 1/0 on some
            // firmwares (benvanmierloo PR #9). The default-coalesce `| 0`
            // returns 0 on a bool true since ArduinoJson treats them as
            // different types — cast through bool then int to be safe.
            merged["power_sharing"] = d["r"]["dyps"].as<bool>() ? 1 : 0;
        }
    }
    if (_state != State::CONNECTED) return;

    String r4 = _sendCommandDirect("g_phsw", "null", SETTINGS_TIMEOUT_MS);
    if (!r4.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r4) == DeserializationError::Ok) {
            merged["phase_switch"] = d["r"]["enabled"].as<bool>() ? 1 : 0;
        }
    }
    if (_state != State::CONNECTED) return;

    String r5 = _sendCommandDirect(bapi::MET_GET_TIMEZONE, "null", SETTINGS_TIMEOUT_MS);
    if (!r5.isEmpty()) {
        JsonDocument d; if (deserializeJson(d, r5) == DeserializationError::Ok) {
            const char* tz = d["r"]["timezone"] | "UTC";
            merged["timezone"] = tz;
        }
    }
    merged["halo"] = 2;  // placeholder — no verified getter yet

    String out;
    serializeJson(merged, out);
    _storeCache(_cachedSettingsJson, _seqSettings, out);
}

void WallboxBLE::_pollNotifications() {
    if (_state != State::CONNECTED) return;
    String resp = _sendCommandDirect(bapi::MET_GET_NOTIFS, "null", 3000);
    if (resp.isEmpty()) return;
    JsonDocument d;
    if (deserializeJson(d, resp) != DeserializationError::Ok) return;

    JsonVariantConst arr = d["r"];
    size_t count = arr.is<JsonArrayConst>() ? arr.size() : 0;
    String firstMsg;
    if (count > 0) {
        JsonVariantConst n = arr[0];
        const char* msg = n["message"] | n["msg"] | n["text"] | (const char*)nullptr;
        if (msg) firstMsg = msg;
    }
    JsonDocument out;
    out["count"]  = count;
    out["latest"] = firstMsg;
    out["items"]  = arr;
    String payload;
    serializeJson(out, payload);
    _storeCache(_cachedNotificationsJson, _seqNotifications, payload);
}

void WallboxBLE::copyCachedStatus(String& out, uint32_t& seq) {
    if (!_cacheMutex) { out = ""; seq = 0; return; }
    if (xSemaphoreTake(_cacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = _cachedStatusJson; seq = _seqStatus;
        xSemaphoreGive(_cacheMutex);
    } else { out = ""; seq = 0; }
}
void WallboxBLE::copyCachedRealtime(String& out, uint32_t& seq) {
    if (!_cacheMutex) { out = ""; seq = 0; return; }
    if (xSemaphoreTake(_cacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = _cachedRealtimeJson; seq = _seqRealtime;
        xSemaphoreGive(_cacheMutex);
    } else { out = ""; seq = 0; }
}
void WallboxBLE::copyCachedMeter(String& out, uint32_t& seq) {
    if (!_cacheMutex) { out = ""; seq = 0; return; }
    if (xSemaphoreTake(_cacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = _cachedMeterJson; seq = _seqMeter;
        xSemaphoreGive(_cacheMutex);
    } else { out = ""; seq = 0; }
}
void WallboxBLE::copyCachedSettings(String& out, uint32_t& seq) {
    if (!_cacheMutex) { out = ""; seq = 0; return; }
    if (xSemaphoreTake(_cacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = _cachedSettingsJson; seq = _seqSettings;
        xSemaphoreGive(_cacheMutex);
    } else { out = ""; seq = 0; }
}
void WallboxBLE::copyCachedNotifications(String& out, uint32_t& seq) {
    if (!_cacheMutex) { out = ""; seq = 0; return; }
    if (xSemaphoreTake(_cacheMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out = _cachedNotificationsJson; seq = _seqNotifications;
        xSemaphoreGive(_cacheMutex);
    } else { out = ""; seq = 0; }
}
