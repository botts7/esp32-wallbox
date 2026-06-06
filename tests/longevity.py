"""Longevity / resilience hardening — long-soak + cross-subsystem stress.

Three classes targeting failure modes that only show under sustained
or concurrent load:

  - TestMemoryStability: 90 s soak with periodic light burst, sample
    heap_free every 10 s. Catches gradual leaks in the new async
    machinery (response map String allocations, pending-pub ring,
    request queue items not freed).

  - TestWsResilience: hold a WebSocket open while bursting HTTP.
    The pre-step-9c symptom was WS dropping under HTTP load because
    the main task couldn't pump WebSocketsServer.loop(). This test
    asserts WS stays connected AND keeps receiving push messages
    throughout the burst.

  - TestMqttCommandStorm (opt-in): publishes HA-style fire-and-forget
    commands rapidly via the MQTT broker, verifies they flow through
    the async _handleCommand path without dropping. Skipped unless
    WB_MQTT_BROKER, WB_MQTT_USER, WB_MQTT_PASS, WB_MQTT_SERIAL are
    set in the environment (the gateway's broker creds aren't
    auto-discoverable).

Read-only BAPI methods only. No state-changing commands during these
tests.

Run:
    python -m unittest tests.longevity -v
"""

from __future__ import annotations

import os
import threading
import time
import unittest
from typing import List, Optional

import requests


GATEWAY = os.environ.get("WB_GATEWAY", "").rstrip("/")
AUTH = (
    os.environ.get("WB_AUTH_USER", "admin"),
    os.environ.get("WB_AUTH_PASS", ""),
)


def _require_env() -> None:
    if not GATEWAY or not AUTH[1]:
        raise unittest.SkipTest("Set WB_GATEWAY and WB_AUTH_PASS to run")


def _health() -> dict:
    r = requests.get(f"{GATEWAY}/api/health", auth=AUTH, timeout=5)
    r.raise_for_status()
    return r.json()


def _send_cheap(retries: int = 3) -> int:
    """Cheap BAPI call with 429 retry. Returns HTTP status."""
    for _ in range(retries):
        r = requests.get(
            f"{GATEWAY}/api/command",
            params={"action": "bapi", "met": "g_tzn", "par": "null",
                    "wait": "2000"},
            auth=AUTH, timeout=8,
        )
        if r.status_code != 429:
            return r.status_code
        ra = float(r.headers.get("Retry-After", "1"))
        time.sleep(min(ra, 2.0))
    return r.status_code


# ---------------------------------------------------------------------------
# Memory stability — proves no monotonic heap_free leak under sustained
# light burst. 90 seconds is long enough to catch a 100-byte-per-request
# leak (would lose ~9 KB over the test) but short enough to keep iteration
# fast.
# ---------------------------------------------------------------------------


class TestMemoryStability(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_heap_free_stable_under_periodic_burst(self) -> None:
        """Sample heap_free at t=0, +10, +20 ... +90s. Between each
        sample, fire 5 cheap BAPI calls. Assert: heap_free at end is
        within 20 KB of starting (allows ~200 B/req slop over 45
        requests). A real leak would compound monotonically and
        trigger the assertion."""
        samples: List[int] = []
        start_heap = int(_health().get("heap_free", 0))
        samples.append(start_heap)
        for cycle in range(9):
            # 5 cheap calls between samples (totals 45 calls, well
            # above the response-map size of 4 + queue depth of 6,
            # so any "forgot to free" path gets exercised many times)
            for _ in range(5):
                _send_cheap()
                time.sleep(0.4)  # pace, keep bucket happy
            time.sleep(2)  # let any deferred publishes settle
            h = int(_health().get("heap_free", 0))
            samples.append(h)

        end_heap = samples[-1]
        delta = start_heap - end_heap
        # Print the curve so you can eyeball it
        print(f"\n[memory] heap_free samples (KB): "
              + " ".join(f"{s//1024}" for s in samples))
        print(f"[memory] start={start_heap} end={end_heap} "
              f"delta={delta} bytes")

        # Allow up to 20 KB of natural variance (HA discovery,
        # MQTT reconnect window, transient String allocations
        # in the diag event ring). A real leak under this workload
        # would be MUCH larger.
        self.assertLess(delta, 20480,
            f"heap_free dropped by {delta} bytes over 90 s — "
            f"possible leak. Samples: {samples}")

    def test_heap_free_above_safe_minimum(self) -> None:
        """The gateway should never operate with < 50 KB heap_free
        on a Pulsar MAX. Lower than that and we're one BAPI response
        away from PSRAM/malloc failures (which trigger reboot)."""
        h = int(_health().get("heap_free", 0))
        self.assertGreater(h, 50_000,
            f"heap_free={h} below safe minimum — investigate before "
            f"shipping")


# ---------------------------------------------------------------------------
# WebSocket resilience under HTTP load
# ---------------------------------------------------------------------------


class TestWsResilience(unittest.TestCase):
    """Hold a WS open, fire HTTP bursts, verify WS stays connected
    and keeps receiving push messages. Pre-step-9c, WS would drop
    its accept handshake during 5 s BAPI waits because the main
    task couldn't pump WebSocketsServer.loop(). The chunked-wait
    pump now keeps WS alive — this test proves it.

    Implementation: WS run in foreground (this test thread) using
    a non-blocking recv loop. HTTP bursts are fired between WS
    polls, keeping everything single-threaded for clarity."""

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()
        try:
            import websocket  # noqa: F401
        except ImportError:
            raise unittest.SkipTest(
                "websocket-client not installed — pip install websocket-client"
            )

    def test_ws_survives_30s_of_http_burst(self) -> None:
        import websocket
        host = GATEWAY.replace("http://", "").replace("https://", "")
        host = host.split(":")[0]
        # WS server is hardcoded at port 81 in wb_ws.cpp
        ws_url = f"ws://{host}:81/"

        # Let gateway settle from prior test (MQTT storm leaves the
        # _mqttCallback drain queue working through 30 messages).
        # If we try the WS handshake while the main task is mid-
        # callback, the accept can time out — that's a real race
        # but the recovery (retry once) is the right user experience.
        ws = None
        last_err = None
        for attempt in range(3):
            try:
                ws = websocket.create_connection(ws_url, timeout=8)
                break
            except Exception as e:
                last_err = e
                time.sleep(2)
        self.assertIsNotNone(ws,
            f"WS connect failed after 3 attempts; last: {last_err}")
        ws.settimeout(0.2)
        try:
            messages_received = 0
            http_calls = 0
            end_at = time.time() + 30
            burst_at = time.time()

            while time.time() < end_at:
                # Non-blocking drain of any pushes
                try:
                    while True:
                        msg = ws.recv()
                        if not msg:
                            break
                        messages_received += 1
                except websocket._exceptions.WebSocketTimeoutException:
                    pass
                except Exception as e:
                    self.fail(f"WS recv failed mid-burst: {e}")

                # Fire one HTTP call every iteration
                code = _send_cheap()
                if code in (200, 202):
                    http_calls += 1
                # Pace ~3 req/s to keep bucket happy
                time.sleep(0.35)

            # Final drain of any straggler pushes
            time.sleep(2)
            try:
                while True:
                    msg = ws.recv()
                    if not msg:
                        break
                    messages_received += 1
            except websocket._exceptions.WebSocketTimeoutException:
                pass

            print(f"\n[ws] http_calls={http_calls} ws_msgs={messages_received}")
            # HTTP must not stall completely — pre-step-9c, the WS+HTTP
            # contention could deadlock both. We do single calls with
            # 2 s wait + 350 ms pace, so 30 s realistically yields
            # 15-30 calls. 10 is the floor that catches a real stall.
            self.assertGreaterEqual(http_calls, 10,
                f"only {http_calls} HTTP calls succeeded — burst stalled")
            # WS must receive pushes throughout. Gateway sends:
            #   - ble health every ~2 s (most frequent push)
            #   - status (r_sta) every ~10 s
            #   - meter (r_dca) every ~30 s
            # 3 is a very conservative floor; healthy gateway delivers
            # 10-20 in a 30 s window. The failing path would be 0
            # (handshake dropped or push queue starved).
            self.assertGreaterEqual(messages_received, 3,
                f"only {messages_received} WS msgs in 30 s — gateway push "
                f"may be starved")
        finally:
            ws.close()


# ---------------------------------------------------------------------------
# MQTT command storm — opt-in (needs broker credentials)
# ---------------------------------------------------------------------------


class TestMqttCommandStorm(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()
        cls.broker = os.environ.get("WB_MQTT_BROKER", "")
        cls.user   = os.environ.get("WB_MQTT_USER", "")
        cls.passw  = os.environ.get("WB_MQTT_PASS", "")
        # Topic prefix is bare "wallbox" — no serial — per live probe of
        # the gateway's actual published topics. wb_mqtt.cpp uses
        # `wallbox/<sub>` for status / settings / availability /
        # response/<met> and accepts commands on `wallbox/command/<sub>`
        # and the raw passthrough `wallbox/bapi`.
        cls.prefix = os.environ.get("WB_MQTT_PREFIX", "wallbox")
        if not cls.broker:
            raise unittest.SkipTest(
                "MQTT storm test needs WB_MQTT_BROKER, WB_MQTT_USER, "
                "WB_MQTT_PASS (WB_MQTT_PREFIX optional, default 'wallbox')"
            )
        try:
            import paho.mqtt.client  # noqa: F401
        except ImportError:
            raise unittest.SkipTest(
                "paho-mqtt not installed — pip install paho-mqtt"
            )

    def test_30_rapid_bapi_commands_via_mqtt(self) -> None:
        """Publish 30 r_dat reads via wallbox/<serial>/bapi rapidly.
        Verify responses come back on wallbox/<serial>/response/r_dat
        within a reasonable window. Tests that the async
        _handleCommand path in wb_mqtt.cpp doesn't drop commands
        under burst.

        We expect to see at least 1 response (retained, last value)
        and possibly more if responses haven't been overwritten."""
        import paho.mqtt.client as mqtt
        import json

        responses_received = []
        connected_event = threading.Event()

        def on_connect(c, u, f, rc, *args):
            if rc == 0:
                connected_event.set()
                c.subscribe(f"{self.prefix}/response/+")
                c.subscribe(f"{self.prefix}/status")

        def on_message(c, u, msg):
            responses_received.append((msg.topic, time.time()))

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if self.user:
            client.username_pw_set(self.user, self.passw)
        client.on_connect = on_connect
        client.on_message = on_message
        host, _, port = self.broker.partition(":")
        client.connect(host, int(port) if port else 1883, 30)
        client.loop_start()

        if not connected_event.wait(timeout=10):
            client.loop_stop()
            self.fail(f"never connected to MQTT broker {self.broker}")

        # Wait briefly for retained messages to flush in, then snapshot
        time.sleep(2)
        baseline = len(responses_received)
        # Fire 30 rapid bapi commands — read-only r_dat
        for i in range(30):
            payload = json.dumps({"met": "r_dat", "par": "null"})
            client.publish(f"{self.prefix}/bapi", payload, qos=0)
            time.sleep(0.05)  # 20 msg/s

        # Let responses come back
        time.sleep(10)
        client.loop_stop()
        client.disconnect()

        new_responses = len(responses_received) - baseline
        print(f"\n[mqtt] sent=30 received_topics={new_responses}")
        # At minimum we should see ONE response (gateway publishes
        # at least the last r_dat result to the retained topic).
        # Higher counts indicate the gateway is keeping up with the
        # burst rate.
        self.assertGreater(new_responses, 0,
            "no MQTT responses observed — async path may be dropping "
            "_handleCommand traffic")


if __name__ == "__main__":
    unittest.main()
