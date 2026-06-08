#pragma once

#include <Arduino.h>
#include <WiFiServer.h>
#include <WiFiClient.h>

// Circular ring buffer of the most recent log output, so /api/logs can
// hand the last few hundred lines back to a web UI for post-incident
// diagnosis without needing a USB cable. Sized to fit comfortably in
// internal SRAM — ~16 KB ceiling.
class TelnetLog : public Print {
public:
    void begin(uint16_t port = 23);
    void loop();

    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t len) override;

    uint8_t clientCount() const { return _clientCount; }

    // Copy the entire ring buffer (oldest-first) into `out`. Returns
    // the number of bytes copied. Safe to call from a web handler.
    size_t copyBuffer(String& out) const;

    // Buffer capacity in bytes (compile-time constant for callers).
    static const size_t BUFFER_SIZE = 16384;

private:
    static const uint8_t MAX_CLIENTS = 2;
    WiFiServer* _server = nullptr;
    WiFiClient  _clients[MAX_CLIENTS];
    uint8_t     _clientCount = 0;
    uint8_t     _lastByte = 0;       // for CR-injection in write() — see .cpp

    // Circular buffer state. _head is the write index; when wrapped,
    // the oldest valid byte is at _head (next-to-be-overwritten slot).
    char     _buf[BUFFER_SIZE];
    size_t   _head = 0;
    bool     _wrapped = false;
};

extern TelnetLog Log;
