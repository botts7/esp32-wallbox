#include "wb_ws.h"
#include "wb_log.h"
#include <ESPAsyncWebServer.h>

namespace wbws {

// 3.0 task #82: migrated from links2004/WebSocketsServer (its own TCP
// listener, briefly on :81, briefly on :82 after step J's port swap)
// to AsyncWebSocket attached to the existing AsyncWebServer on :80.
// Client connects to ws://host/ws — same port as HTTP, no separate
// TCP listener, no port-collision games.
//
// wb_web_async::begin() registers this handler with the AsyncWebServer
// via the wbws::handler() accessor below before calling _async.begin().
static AsyncWebSocket _ws("/ws");

static void onEvent(AsyncWebSocket* /*server*/,
                    AsyncWebSocketClient* client,
                    AwsEventType type,
                    void* /*arg*/,
                    uint8_t* /*data*/,
                    size_t /*len*/) {
    if (type == WS_EVT_CONNECT) {
        Log.printf("[WS] client %u connected from %s\n",
                   client->id(), client->remoteIP().toString().c_str());
    } else if (type == WS_EVT_DISCONNECT) {
        Log.printf("[WS] client %u disconnected\n", client->id());
    }
    // WS_EVT_DATA / WS_EVT_PING / WS_EVT_PONG / WS_EVT_ERROR ignored —
    // we're a push-only server, the library handles ping/pong itself.
}

AsyncWebSocket& handler() { return _ws; }

void begin() {
    _ws.onEvent(onEvent);
    Log.println("[WS] AsyncWebSocket attached at /ws on :80");
}

void loop() {
    // Reap disconnected clients periodically. Cheap — walks the
    // intrusive client list and frees ones whose underlying TCP
    // connection has been gone for > X seconds. AsyncWebSocket
    // recommends calling this from main loop; we already loop().
    _ws.cleanupClients();
}

void broadcast(const char* type, const String& jsonPayload) {
    if (_ws.count() == 0) return;  // skip if nobody listening
    String msg;
    msg.reserve(jsonPayload.length() + 32);
    msg = "{\"t\":\"";
    msg += type;
    msg += "\",\"d\":";
    msg += (jsonPayload.length() ? jsonPayload : "null");
    msg += "}";
    _ws.textAll(msg);
}

void broadcastBleHealth(const char* state, int rssi, uint32_t lastActivitySec) {
    if (_ws.count() == 0) return;
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
    return _ws.count();
}

}  // namespace wbws
