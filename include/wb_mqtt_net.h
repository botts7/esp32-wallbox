#pragma once
// MQTT's network connection, with a breaker for a stalled socket.
//
// On arduino-esp32 2.0.x a WiFiClient::write() into a socket that has stopped
// draining retries select() 10 times at 1 s each (WIFI_CLIENT_MAX_WRITE_RETRY,
// WIFI_CLIENT_SELECT_TIMEOUT_US), whatever setTimeout() says. PubSubClient is
// synchronous and one main-loop pass publishes 8 or more messages, so a
// briefly stalled path to the broker froze the loop for 80-190 s (#74:
// loop_events of 80-90 s, each ending in a broker keepalive timeout), and the
// web UI, WebSocket and OTA with it.
//
// The first write that can't get all its bytes out closes the socket. Every
// later write in the same pass then fails at once (a closed WiFiClient returns
// 0 immediately), PubSubClient sees the disconnect, and the normal reconnect
// runs on the next loop. A stall now costs one write's worth (about 10 s),
// not the whole pass.

#include <WiFiClient.h>

class WbMqttNetClient : public WiFiClient {
public:
    size_t write(const uint8_t* buf, size_t size) override;
    size_t write(uint8_t b) override { return write(&b, 1); }

    // How many times the breaker has closed the socket since boot.
    uint32_t stalls() const { return _stalls; }

private:
    uint32_t _stalls = 0;
};
