#include "wb_ws.h"
#include "wb_log.h"
#include <WebSocketsServer.h>

namespace wbws {

// 3.0 task #78 step J port-swap fix: WS lived on port 81 historically
// (so it didn't collide with sync HTTP on 80). After step J flipped
// the sync server onto 81, both wanted the same TCP port and sync
// won the bind — WS upgrade handshakes were getting 200 OK from the
// HTTP server instead of the 101 Switching Protocols they needed.
// Moved to 82 to dodge the collision; client JS in wb_web.cpp
// updated to match. Full AsyncWebSocket migration is still on the
// to-do list but not blocking this fix.
static WebSocketsServer _ws(82);

static void onEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) {
        IPAddress ip = _ws.remoteIP(num);
        Log.printf("[WS] client %u connected from %s\n", num, ip.toString().c_str());
    } else if (type == WStype_DISCONNECTED) {
        Log.printf("[WS] client %u disconnected\n", num);
    } else if (type == WStype_PING || type == WStype_PONG) {
        // ignore — library handles automatically
    }
}

void begin() {
    _ws.begin();
    _ws.onEvent(onEvent);
    Log.println("[WS] server listening on :81");
}

void loop() {
    _ws.loop();
}

void broadcast(const char* type, const String& jsonPayload) {
    if (_ws.connectedClients() == 0) return;  // skip if nobody listening
    String msg;
    msg.reserve(jsonPayload.length() + 32);
    msg = "{\"t\":\"";
    msg += type;
    msg += "\",\"d\":";
    msg += (jsonPayload.length() ? jsonPayload : "null");
    msg += "}";
    _ws.broadcastTXT(msg);
}

void broadcastBleHealth(const char* state, int rssi, uint32_t lastActivitySec) {
    if (_ws.connectedClients() == 0) return;
    String j = "{\"state\":\"";
    j += state;
    j += "\",\"rssi\":";
    j += String(rssi);
    j += ",\"last_activity_s\":";
    j += String(lastActivitySec);
    j += "}";
    broadcast("ble", j);
}

size_t clientCount() {
    return _ws.connectedClients();
}

}  // namespace wbws
