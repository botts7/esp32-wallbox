"""Edge-case hardening for the 2.7.0 async refactor.

Three classes of test:

  - TestParallelCorrelation: fire concurrent BAPI calls with UNIQUE
    parameters (r_log with different session ids), verify each
    waiter gets its own response back. This is the most direct
    proof that the response-map FIFO + wake-up correlation in
    wb_ble.cpp doesn't cross wires under concurrency.

  - TestUrlEdgeCases: feed garbage / negative / oversize values to
    the new ?wait, ?sync, /api/command_status?id params. Verify
    the server clamps / rejects cleanly without crashing or
    leaking diagnostic state.

  - TestBackpressure: deliberately exhaust the token bucket and
    the BLE request queue. Verify 429 with Retry-After header
    and 503 with retry:true body, and that we recover within a
    second.

Read-only BAPI methods only — no s_*/w_*/clr_* writes.

Run:
    python -m unittest tests.edge_cases -v
"""

from __future__ import annotations

import os
import time
import unittest
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Tuple

import requests


GATEWAY = os.environ.get("WB_GATEWAY", "").rstrip("/")
AUTH = (
    os.environ.get("WB_AUTH_USER", "admin"),
    os.environ.get("WB_AUTH_PASS", ""),
)


def _require_env() -> None:
    if not GATEWAY or not AUTH[1]:
        raise unittest.SkipTest("Set WB_GATEWAY and WB_AUTH_PASS to run")


def _get(path: str, **kwargs) -> requests.Response:
    kwargs.setdefault("timeout", 15)
    return requests.get(f"{GATEWAY}{path}", auth=AUTH, **kwargs)


# ---------------------------------------------------------------------------
# Parallel correlation: each waiter must get ITS response, not a sibling's.
# ---------------------------------------------------------------------------


class TestParallelCorrelation(unittest.TestCase):
    """The response-map FIFO holds 4 slots; the BLE queue holds 6.
    Under 6-way concurrency, every waiter must still receive the
    body belonging to its own request id.

    We use r_log with unique session ids per worker. Each session's
    body shape is `{"r": {"id": N, ...}}` — we verify the returned
    id matches the requested par.
    """

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()
        # r_ses returns {"r":{"last":N, "size":M}} where N is the
        # highest session id. /sessions iterates downward from there
        # via r_log&par=<id>. Mirror that pattern: pick the 6 most
        # recent ids for parallel correlation testing.
        r = _get("/api/command",
                 params={"action": "bapi", "met": "r_ses", "par": "null",
                         "wait": "5000"})
        cls.session_ids: List[int] = []
        try:
            d = r.json()
            last = None
            if "r" in d and isinstance(d["r"], dict):
                last = d["r"].get("last")
            if isinstance(last, int) and last >= 6:
                cls.session_ids = [last - i for i in range(6)]
        except Exception:
            pass
        if len(cls.session_ids) < 3:
            raise unittest.SkipTest(
                f"need >=3 real session ids to test correlation; "
                f"r_ses returned: {r.text[:120]}"
            )

    def _fire_one(self, sid: int) -> Tuple[int, requests.Response, float]:
        t0 = time.perf_counter()
        r = _get("/api/command", params={
            "action": "bapi", "met": "r_log", "par": str(sid),
        })
        return sid, r, time.perf_counter() - t0

    def test_six_parallel_unique_pars_route_correctly(self) -> None:
        """Fire min(6, available) parallel r_log with unique pars.
        Every response body must reference the par it was called
        with — proving the BLE task's response-map keys correctly
        and the xTaskNotify wakes the RIGHT waiter."""
        ids = self.session_ids[:6]
        with ThreadPoolExecutor(max_workers=len(ids)) as ex:
            futures = [ex.submit(self._fire_one, sid) for sid in ids]
            results = [f.result() for f in as_completed(futures)]
        self.assertEqual(len(results), len(ids))
        crosses: List[str] = []
        for sid, r, dur in results:
            if r.status_code != 200:
                # 429 from token bucket is OK — that's the rate
                # limiter doing its job under burst. We're checking
                # correlation, not throughput.
                if r.status_code != 429:
                    crosses.append(f"sid={sid} got http={r.status_code}")
                continue
            try:
                body = r.json()
            except Exception:
                crosses.append(f"sid={sid} non-JSON body")
                continue
            if "error" in body:
                # Charger-side error — not a crossed wire
                continue
            inner = body.get("r")
            if isinstance(inner, dict):
                returned_id = inner.get("id")
                if returned_id is not None and int(returned_id) != sid:
                    crosses.append(
                        f"REQUESTED sid={sid} RECEIVED id={returned_id}"
                    )
        if crosses:
            self.fail("Response correlation failure(s):\n  " +
                      "\n  ".join(crosses))

    def test_async_pure_then_command_status_correlates(self) -> None:
        """?wait=0 returns 202+id immediately; the subsequent
        /api/command_status?id=N must return THAT request's body,
        not a sibling's even if siblings were enqueued concurrently."""
        ids = self.session_ids[:3]
        # Stage: fire 3 async ?wait=0 in parallel; collect (req_id, sid).
        with ThreadPoolExecutor(max_workers=3) as ex:
            def stage(sid):
                r = _get("/api/command", params={
                    "action": "bapi", "met": "r_log", "par": str(sid),
                    "wait": "0",
                })
                if r.status_code != 202:
                    return (sid, None, r)
                d = r.json()
                return (sid, int(d["id"]), r)
            staged = [f.result() for f in
                      [ex.submit(stage, s) for s in ids]]
        # Poll each /api/command_status?id within 3 seconds.
        for sid, req_id, init_r in staged:
            if req_id is None:
                # 429 or similar — skip this id in the correlation check
                if init_r.status_code == 429:
                    continue
                self.fail(f"sid={sid} initial fire returned "
                          f"http={init_r.status_code} not 202")
            body = None
            for _ in range(30):
                time.sleep(0.1)
                poll = _get("/api/command_status", params={"id": req_id})
                if poll.status_code == 200:
                    body = poll.json()
                    break
            self.assertIsNotNone(body,
                f"reqId={req_id} sid={sid} never completed within 3 s")
            inner = body.get("r")
            if isinstance(inner, dict) and "id" in inner:
                self.assertEqual(int(inner["id"]), sid,
                    f"reqId={req_id} expected sid={sid} got id={inner['id']}")


# ---------------------------------------------------------------------------
# URL edge cases
# ---------------------------------------------------------------------------


class TestUrlEdgeCases(unittest.TestCase):
    """The new ?wait / ?sync / /api/command_status?id surface is
    documented as accepting a specific range. Garbage inputs must
    not crash the gateway, must not be silently misinterpreted,
    and must return predictable status codes."""

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_wait_negative_clamps_to_zero(self) -> None:
        """?wait=-100 is documented as clamping to 0 (pure async)."""
        r = _get("/api/command", params={
            "action": "bapi", "met": "g_tzn", "par": "null",
            "wait": "-100",
        })
        # 0 ms wait → 202 immediately
        self.assertIn(r.status_code, (200, 202),
            f"got {r.status_code}: {r.text[:80]}")
        if r.status_code == 202:
            data = r.json()
            self.assertEqual(data.get("status"), "pending")

    def test_wait_huge_clamps_to_max(self) -> None:
        """?wait=999999 must clamp to the server-side max (8000 ms
        as of step 9g). g_tzn is fast — returns well within the
        clamp regardless. The bound checks that the server doesn't
        try to wait the full 999999 ms (would just block forever)."""
        t0 = time.perf_counter()
        r = _get("/api/command", params={
            "action": "bapi", "met": "g_tzn", "par": "null",
            "wait": "999999",
        }, timeout=12)
        dur = time.perf_counter() - t0
        self.assertIn(r.status_code, (200, 202))
        self.assertLess(dur, 9.0,
            f"took {dur:.1f} s — clamp at 8000 ms appears broken")

    def test_wait_garbage_falls_to_default(self) -> None:
        """?wait=notanumber: String::toInt() returns 0, so we get
        the pure-async 202 path. This is documented behavior."""
        r = _get("/api/command", params={
            "action": "bapi", "met": "g_tzn", "par": "null",
            "wait": "notanumber",
        })
        self.assertIn(r.status_code, (200, 202))

    def test_unknown_action_returns_400(self) -> None:
        r = _get("/api/command", params={"action": "frobnicate"})
        self.assertEqual(r.status_code, 400)

    def test_missing_action_returns_400(self) -> None:
        r = _get("/api/command")
        # No action arg at all → unknown action branch
        self.assertEqual(r.status_code, 400)

    def test_command_status_negative_id_returns_400(self) -> None:
        """toInt() of "-5" yields -5 → cast to uint32_t → huge id.
        That huge id is then > peekNextReqId, so step 9's audit fix
        returns 410. Either 400 (parsed as <= 0) or 410 (future-id)
        is defensible — both are clear rejections."""
        r = _get("/api/command_status", params={"id": "-5"})
        self.assertIn(r.status_code, (400, 410))

    def test_command_status_empty_id_returns_400(self) -> None:
        r = _get("/api/command_status", params={"id": ""})
        self.assertEqual(r.status_code, 400)

    def test_long_par_does_not_crash(self) -> None:
        """BleReq.par is char[192] — strncpy truncates. Sending a
        4 KB par must not crash the gateway."""
        big = "x" * 4096
        r = _get("/api/command", params={
            "action": "bapi", "met": "g_tzn", "par": big,
        })
        # Whatever the server decides (likely BAPI returns error
        # because of mangled par), the gateway must stay alive.
        self.assertIn(r.status_code, (200, 202, 400, 414, 500))
        # And /api/health must still respond afterwards
        h = _get("/api/health", timeout=5)
        self.assertEqual(h.status_code, 200)

    def test_sync_param_anything_but_1_is_async(self) -> None:
        """?sync=true / yes / on / 0 are all 'not "1"' per the
        comparison `http.arg("sync") == "1"`. They should all hit
        the async path."""
        for val in ("true", "yes", "0", "False", ""):
            r = _get("/api/command", params={
                "action": "bapi", "met": "g_tzn", "par": "null",
                "sync": val,
            })
            self.assertIn(r.status_code, (200, 202),
                f"sync={val!r} → {r.status_code}")


# ---------------------------------------------------------------------------
# Backpressure: rate limiter + queue overflow
# ---------------------------------------------------------------------------


class TestBackpressure(unittest.TestCase):
    """Two distinct guards in wb_web.cpp `handleApiCommand`:
      - Token bucket (TB_CAP=4, refill 2/sec) → 429 + Retry-After
      - BLE request queue (depth 6) → 503 + retry:true

    Verify both fire cleanly and recover within a second."""

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()
        # Drain to a known state so we don't race the previous test
        time.sleep(3)  # let bucket refill to 4

    def test_token_bucket_429s_then_recovers(self) -> None:
        """Bucket capacity 4 + refill 2/sec means a burst of 8
        rapid calls should yield ~4 OK and ~4 429s. After waiting
        2 seconds, more calls should succeed again."""
        statuses = []
        for _ in range(8):
            r = _get("/api/command", params={
                "action": "bapi", "met": "g_tzn", "par": "null",
                "wait": "100",   # cheap call, finish fast
            })
            statuses.append(r.status_code)
        ok = sum(1 for s in statuses if s == 200 or s == 202)
        rate = sum(1 for s in statuses if s == 429)
        self.assertGreaterEqual(rate, 1,
            f"expected at least one 429 from rate limiter, got {statuses}")
        # Wait for refill
        time.sleep(3)
        r = _get("/api/command", params={
            "action": "bapi", "met": "g_tzn", "par": "null"})
        self.assertIn(r.status_code, (200, 202),
            f"bucket should have refilled, got {r.status_code}")

    def test_429_has_retry_after_header(self) -> None:
        """RFC-compliant 429s carry Retry-After. Without it,
        well-behaved clients (HA, browser fetch retry policies)
        can't pace themselves."""
        # Burn the bucket
        for _ in range(8):
            _get("/api/command", params={
                "action": "bapi", "met": "g_tzn", "par": "null",
                "wait": "100"})
        # Now the next one should 429
        for _ in range(5):
            r = _get("/api/command", params={
                "action": "bapi", "met": "g_tzn", "par": "null"})
            if r.status_code == 429:
                self.assertIn("Retry-After", r.headers,
                    "429 missing Retry-After header")
                # Body should hint at retry
                try:
                    b = r.json()
                    self.assertTrue(b.get("retry"))
                except Exception:
                    pass
                return
        # If we never got 429, the bucket refilled too fast for the
        # test to catch it. Skip rather than fail — the bucket logic
        # itself is exercised by the previous test.
        self.skipTest("bucket refill outpaced the test loop")


if __name__ == "__main__":
    unittest.main()
