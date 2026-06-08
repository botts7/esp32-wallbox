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

// Telnet RFC 854 requires CRLF for line endings. Serial Monitor / IDE
// terminals are forgiving with LF-only, but raw TCP clients on Windows
// (built-in telnet.exe, PowerShell, etc.) render unreadably without it.
// We mirror raw bytes to Serial, but inject \r before any unpaired \n
// when writing to telnet clients.

size_t TelnetLog::write(uint8_t c) {
    Serial.write(c);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (_clients[i] && _clients[i].connected()) {
            if (c == '\n' && _lastByte != '\r') _clients[i].write('\r');
            _clients[i].write(c);
        }
    }
    _lastByte = c;
    // Append to the in-RAM ring buffer (raw byte — no CR injection, so
    // /api/logs returns clean LF-terminated text the web can render).
    _buf[_head] = (char)c;
    _head = (_head + 1) % BUFFER_SIZE;
    if (_head == 0) _wrapped = true;
    return 1;
}

size_t TelnetLog::write(const uint8_t* buf, size_t len) {
    Serial.write(buf, len);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!_clients[i] || !_clients[i].connected()) continue;
        size_t start = 0;
        uint8_t prev = _lastByte;
        for (size_t j = 0; j < len; j++) {
            if (buf[j] == '\n' && prev != '\r') {
                if (j > start) _clients[i].write(buf + start, j - start);
                _clients[i].write('\r');
                start = j;  // re-emit the \n below as part of next run
            }
            prev = buf[j];
        }
        if (start < len) _clients[i].write(buf + start, len - start);
    }
    if (len > 0) _lastByte = buf[len - 1];
    // Mirror into ring buffer
    for (size_t i = 0; i < len; i++) {
        _buf[_head] = (char)buf[i];
        _head = (_head + 1) % BUFFER_SIZE;
        if (_head == 0) _wrapped = true;
    }
    return len;
}

size_t TelnetLog::copyBuffer(String& out) const {
    out = "";
    if (!_wrapped) {
        if (_head == 0) return 0;
        out.reserve(_head);
        for (size_t i = 0; i < _head; i++) out += _buf[i];
        return _head;
    }
    // Wrapped: oldest byte is at _head, newest is just before it.
    // Walk forward from _head and we recover chronological order.
    out.reserve(BUFFER_SIZE);
    for (size_t i = 0; i < BUFFER_SIZE; i++) {
        size_t idx = (_head + i) % BUFFER_SIZE;
        out += _buf[idx];
    }
    return BUFFER_SIZE;
}
