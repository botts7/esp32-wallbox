#include "wb_log.h"

TelnetLog Log;

void TelnetLog::begin(uint16_t port) {
    _server = new WiFiServer(port);
    _server->begin();
    _server->setNoDelay(true);
    Serial.printf("[Telnet] Listening on port %u\n", port);
}

void TelnetLog::loop() {
    if (!_server) return;

    // Accept new connections
    if (_server->hasClient()) {
        WiFiClient incoming = _server->available();
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!_clients[i] || !_clients[i].connected()) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            _clients[slot] = incoming;
            _clients[slot].setNoDelay(true);
            _clients[slot].printf("=== Wallbox Gateway telnet log ===\r\n");
            Serial.printf("[Telnet] Client connected from %s (slot %d)\n",
                incoming.remoteIP().toString().c_str(), slot);
        } else {
            incoming.printf("Max clients reached, goodbye.\r\n");
            incoming.stop();
        }
    }

    // Clean up disconnected clients and discard input from live ones
    _clientCount = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!_clients[i]) continue;
        if (!_clients[i].connected()) {
            _clients[i].stop();
            continue;
        }
        _clientCount++;
        while (_clients[i].available()) _clients[i].read();
    }
}

size_t TelnetLog::write(uint8_t c) {
    Serial.write(c);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected()) {
            _clients[i].write(c);
        }
    }
    return 1;
}

size_t TelnetLog::write(const uint8_t* buf, size_t len) {
    Serial.write(buf, len);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected()) {
            _clients[i].write(buf, len);
        }
    }
    return len;
}
