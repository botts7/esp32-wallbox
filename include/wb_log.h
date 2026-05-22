#pragma once

#include <Arduino.h>
#include <WiFiServer.h>
#include <WiFiClient.h>

class TelnetLog : public Print {
public:
    void begin(uint16_t port = 23);
    void loop();

    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t len) override;

    uint8_t clientCount() const { return _clientCount; }

private:
    static const uint8_t MAX_CLIENTS = 2;
    WiFiServer* _server = nullptr;
    WiFiClient  _clients[MAX_CLIENTS];
    uint8_t     _clientCount = 0;
};

extern TelnetLog Log;
