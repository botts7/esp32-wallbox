#include "wb_ble.h"
#include "wb_log.h"
#include <ArduinoJson.h>
#include <esp_coexist.h>

WallboxBLE wallboxBLE;
WallboxBLE* WallboxBLE::_instance = nullptr;

static const char* DEF_SVC = "2456e1b9-26e2-8f83-e744-f34f01e9d701";
static const char* DEF_CHR = "2456e1b9-26e2-8f83-e744-f34f01e9d703";

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
}

void WallboxBLE::pause(uint32_t ms) {
    _pausedUntil = millis() + ms;
    if (_state == State::CONNECTED) _disconnect();
    _state = State::DISCONNECTED;
    Log.printf("[BLE] paused for %u ms (release for official app)\n", ms);
}

void WallboxBLE::loop() {
    if (isPaused()) return;  // skip all BLE activity during user-requested pause
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

        // Keepalive ping if idle (no commands sent recently)
        if (millis() - _lastActivityTime >= PING_INTERVAL_MS) {
            String resp = sendCommand(bapi::MET_PING, "null", 2000);
            if (resp.isEmpty()) {
                Log.println("[BLE] Ping timeout — connection dead, reconnecting");
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
        break;

    case State::CONNECTING:
    case State::AUTHENTICATING:
        break;
    }
}

void WallboxBLE::_connect() {
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

    // Discover services
    NimBLERemoteService* svc = _client->getService(_svcUUID.c_str());
    if (!svc) {
        Log.printf("[BLE] Service %s not found\n", _svcUUID.c_str());
        _client->disconnect();
        _state = State::ERROR;
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        return;
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

    // Subscribe to notifications — if CCCD write is rejected, encrypt and retry
    bool notifyOk = notifyChr->canNotify() && notifyChr->registerForNotify(_notifyCb);
    if (!notifyOk && notifyChr->canNotify()) {
        Log.println("[BLE] CCCD rejected, trying SMP encryption...");
        delay(200);
        if (_client->secureConnection()) {
            Log.println("[BLE] Encrypted, retrying notifications...");
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

    // Read GATT Device Info service (0x180A) if present
    NimBLERemoteService* devInfo = _client->getService("180a");
    if (devInfo) {
        NimBLERemoteCharacteristic* c;
        if ((c = devInfo->getCharacteristic("2a29")) != nullptr) _devMfg = c->readValue().c_str();
        if ((c = devInfo->getCharacteristic("2a24")) != nullptr) _devModel = c->readValue().c_str();
        if ((c = devInfo->getCharacteristic("2a26")) != nullptr) _devFw = c->readValue().c_str();
        Log.printf("[BLE] Device: %s / %s / FW %s\n",
            _devMfg.c_str(), _devModel.c_str(), _devFw.c_str());
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

    // Authenticate if needed
    if (_authenticate()) {
        _state = State::CONNECTED;
        Log.println("[BLE] Ready");
    }
}

void WallboxBLE::_disconnect() {
    if (_client && _client->isConnected()) {
        _client->disconnect();
    }
    _chr = nullptr;
    _state = State::DISCONNECTED;
}

bool WallboxBLE::_authenticate() {
    _state = State::AUTHENTICATING;

    Log.println("[BLE] Checking PIN status...");
    String pinResp = sendCommand(bapi::MET_READ_PIN, "null", 5000);

    if (pinResp.isEmpty()) {
        Log.println("[BLE] No response to read_pin, assuming no PIN needed");
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

    String authResp = sendCommand(bapi::MET_SET_PIN, par.c_str(), 5000);
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

    bool complete = _instance->_parser.feed(data, len);
    if (complete) {
        _instance->_lastResponse = _instance->_parser.json();
        _instance->_responseReady = true;
        if (_instance->_responseCb) {
            _instance->_responseCb(_instance->_lastResponse);
        }
    }
}

String WallboxBLE::sendCommand(const char* met, const char* par, uint32_t timeoutMs) {
    // Connection health check before sending
    if (!_chr || !_client || !_client->isConnected()) {
        if (_state == State::CONNECTED) {
            Log.printf("[BLE] Connection lost (detected before %s)\n", met);
            _state = State::DISCONNECTED;
            _chr = nullptr;
        }
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
        return "";
    }

    Log.printf("[BLE] RX %s (%d bytes)\n", met, _lastResponse.length());
    return _lastResponse;
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
    if (_client && _client->isConnected()) {
        return _client->getRssi();
    }
    return _scanRSSI;  // Fall back to last scan RSSI
}
