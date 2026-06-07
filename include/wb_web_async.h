#pragma once

#include <Arduino.h>

// 3.0 task #78 — async web server migration scaffolding.
//
// Lives behind the WB_ASYNC_WEB compile flag (default 0). When the
// flag is 0 this header is a no-op — the begin()/loop() bodies are
// empty stubs and the sync WebServer in wb_web.cpp continues to
// serve everything on port 80.
//
// When the flag is 1, an `AsyncWebServer` instance starts on port
// 8081 alongside the existing sync server on port 80. During the
// migration window each route migrates one-by-one to the async
// server; when all 40 routes are migrated, the port-80 sync server
// is retired and the async server moves to port 80. See
// docs/plans/3.x-async-webserver.md for the full migration plan.
//
// Threading: the AsyncWebServer dispatches handlers on the AsyncTCP
// task (a dedicated FreeRTOS task), NOT the main loop. Handlers
// MUST NOT touch PubSubClient directly; cross-task BAPI access
// goes through the wallboxBLE async queue from task #71.

namespace wb_web_async {

// Initialise the async server when WB_ASYNC_WEB is non-zero. Safe
// to call when the flag is 0 — body compiles to a no-op stub.
// Call AFTER WiFi has GOT_IP (wb_net::begin() returns true) so the
// TCP listener can actually bind.
void begin();

}  // namespace wb_web_async
