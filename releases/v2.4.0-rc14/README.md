# v2.4.0-rc14

**Fixes the periodic MQTT-disconnect issue** that several testers (incl.
botts7 and peter-mcc on issues #4/#6) saw in HA as entities going
`unavailable` briefly every ~95 seconds.

## What was happening

Mosquitto broker log on rc12 showed a beautifully consistent pattern:

```
17:12:24 wallbox-gw disconnected: connection closed by client
17:12:34 wallbox-gw reconnected         ← 10s gap
17:14:00 wallbox-gw disconnected        ← 86s connected
17:14:10 wallbox-gw reconnected
17:15:35 wallbox-gw disconnected        ← 85s connected
[ ... 12 cycles, every ~95s ... ]
```

rc13's new disconnect counters then confirmed:

```
ble_reconnects:   0
mqtt_reconnects:  12
mqtt_longest_s:   43
```

BLE was rock-solid. MQTT was flapping, and the gateway was *closing the
connection itself* every 85 seconds of connected time.

## Root cause

`pollSettings()` runs every 30 seconds and issues 5 sequential BAPI
commands back-to-back, each capable of blocking the main loop for up
to ~3 seconds waiting for the BLE response. That's up to ~15 seconds
of main-loop blocking per cycle.

During those BLE waits, the yield callback in `main.cpp` ran
`webServer.loop()` and `ArduinoOTA.handle()` — keeping the web UI
responsive — **but not `wallboxMQTT.loop()`**. PubSubClient's
keepalive logic only runs when `loop()` is called.

So MQTT keepalive was getting starved during BAPI chains. After enough
missed keepalive intervals (defaulted to 15s), PubSubClient closes the
socket itself, then our `WallboxMQTT::loop()` notices it's
disconnected and reconnects on the 5s retry timer.

## Fix (two layers)

1. **`main.cpp` BLE yield callback now also calls `wallboxMQTT.loop()`**
   — this is the primary fix. The keepalive logic now gets cycles
   even during a chain of BAPI waits.
2. **`wb_mqtt.cpp` `begin()` now calls `setKeepAlive(60)`**, widening
   PubSubClient's keepalive from the 15s default. Defense-in-depth
   against any transient blockage the yield-callback fix doesn't
   cover.

No behaviour change beyond keepalive timing.

## Verification

After upgrading to rc14, your `/info` Connection Diagnostics card
should show `mqtt_reconnects: 0` and stay there. If it climbs after
a day or two, something else is going on and we'll need to dig deeper
— but the broker-log evidence pointed unambiguously at this
starvation pattern.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11).
Fresh USB installs via `install.json`.
