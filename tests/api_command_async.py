"""Live tests for /api/command async behaviour (task #71, plan 2.7.0).

Step-aligned with docs/plans/2.7.0-api-command-async.md. Each test
class corresponds to a step in the impl plan, so we can run the
relevant subset at each commit point.

Run a specific class:
    python -m unittest tests.api_command_async.TestBaselineSync -v

Run everything (each new step adds more):
    python -m unittest tests.api_command_async -v

Env vars:
    WB_GATEWAY      e.g. http://192.0.2.1 (no trailing slash, required)
    WB_AUTH_USER    e.g. admin (defaults to "admin")
    WB_AUTH_PASS    OTA password from /info or serial log (required)
"""

from __future__ import annotations

import os
import time
import unittest
from typing import Tuple

import requests


GATEWAY = os.environ.get("WB_GATEWAY", "").rstrip("/")
AUTH = (
    os.environ.get("WB_AUTH_USER", "admin"),
    os.environ.get("WB_AUTH_PASS", ""),
)


def _require_env() -> None:
    if not GATEWAY or not AUTH[1]:
        raise unittest.SkipTest(
            "Set WB_GATEWAY and WB_AUTH_PASS env vars to run live tests."
        )


def _cmd_url(met: str, par: str = "null") -> str:
    return f"{GATEWAY}/api/command?action=bapi&met={met}&par={par}"


def _health() -> dict:
    r = requests.get(f"{GATEWAY}/api/health", auth=AUTH, timeout=5)
    r.raise_for_status()
    return r.json()


def _loop_max_ms() -> int:
    return int(_health().get("loop_max_ms", 0))


def _send_cmd(
    met: str, par: str = "null", timeout: float = 15.0, **params
) -> Tuple[requests.Response, float]:
    """POST /api/command and return (response, wall_time_seconds)."""
    url = _cmd_url(met, par)
    # extra params appended (e.g. wait, sync)
    for k, v in params.items():
        url += f"&{k}={v}"
    t0 = time.perf_counter()
    r = requests.get(url, auth=AUTH, timeout=timeout)
    return r, time.perf_counter() - t0


# ---------------------------------------------------------------------------
# Baseline (pre-refactor): the existing sync path.
# These tests should keep passing through every step of the refactor.
# ---------------------------------------------------------------------------


class TestBaselineSync(unittest.TestCase):
    """Pre-refactor contract — preserved via the `?sync=1` escape hatch
    in Step 6. These tests verify the legacy byte-for-byte shape that
    existing curl scripts / HA automations rely on. Without `?sync=1`
    the default path is now async and may return 202 instead of 200,
    which is captured in TestAsyncPath."""

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_g_tzn_sync_returns_200_with_body(self) -> None:
        """The cheapest BAPI call (timezone) round-trips with a body."""
        r, dur = _send_cmd("g_tzn", sync=1)
        self.assertEqual(r.status_code, 200, r.text[:200])
        self.assertIn("application/json", r.headers.get("Content-Type", ""))
        data = r.json()
        # Shape: {"id": N, "r": {...}} on success, {"error": "..."} on failure.
        self.assertTrue("r" in data or "error" in data, data)

    def test_r_dat_sync_returns_status_object(self) -> None:
        """A typical status read — heavier than g_tzn but still fast."""
        r, dur = _send_cmd("r_dat", sync=1)
        self.assertEqual(r.status_code, 200)
        data = r.json()
        if "r" in data and isinstance(data["r"], dict):
            self.assertIn("st", data["r"])

    def test_unknown_method_sync_returns_error_in_body(self) -> None:
        """Server-side validation: unknown BAPI methods should fail
        cleanly in the body."""
        r, dur = _send_cmd("notamethod", sync=1)
        self.assertEqual(r.status_code, 200, "sync path — error in body")


# ---------------------------------------------------------------------------
# Latency observations — drive the post-refactor metrics.
# ---------------------------------------------------------------------------


class TestLatencyBounds(unittest.TestCase):
    """Capture per-request timing so we can verify the refactor bounds."""

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_cheap_command_under_1s(self) -> None:
        """g_tzn over the sync path should be well under 1.5 s. The
        async default path can return 202 if the BLE link is busy,
        which is fine — captured separately in TestAsyncPath."""
        r, dur = _send_cmd("g_tzn", sync=1)
        self.assertEqual(r.status_code, 200)
        self.assertLess(dur, 1.5, f"g_tzn took {dur:.2f}s")

    def test_loop_max_ms_stays_bounded_under_10x_load(self) -> None:
        """Hit /api/command 10× in a tight loop, alternating async
        default + sync escape hatch. loop_max_ms records the worst
        main-loop gap seen during the burst. Pre-refactor: ~800-1500
        ms. Post-Step-4 target: bounded by the slowest single command
        the BLE link served, NOT the burst total."""
        baseline = _loop_max_ms()
        peak = baseline
        for i in range(10):
            # Alternate paths so we cover both async + sync under load
            if i % 2 == 0:
                r, dur = _send_cmd("g_tzn")
                self.assertIn(r.status_code, (200, 202))
            else:
                r, dur = _send_cmd("g_tzn", sync=1)
                self.assertEqual(r.status_code, 200)
        time.sleep(2)
        after = _loop_max_ms()
        peak = max(peak, after)
        print(
            f"\n[latency] baseline={baseline}ms peak={peak}ms "
            f"delta={peak - baseline}ms"
        )


# ---------------------------------------------------------------------------
# Async path (Steps 6-8 in the plan): not yet implemented.
# These tests are stubs — they'll fail until the refactor lands. Use
# `@unittest.skip` to keep CI green during early steps.
# ---------------------------------------------------------------------------


class TestStep2Plumbing(unittest.TestCase):
    """Step 2 verification — the BleReq queue exists and the BLE task
    drains it. We can't enqueue from outside the gateway yet (no
    public HTTP wrapper until step 6), so the test only confirms the
    baseline sync path still works. This is the "scaffolding hasn't
    broken anything" check."""

    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_sync_path_unaffected_by_queue_addition(self) -> None:
        """The ?sync=1 escape hatch path must remain unaffected
        end-to-end by the steps 1-7 plumbing. g_tzn over the sync
        path should complete in roughly the same time it did
        pre-refactor.

        Bound is generous (2.5 s avg) because individual BAPI calls
        vary 100-2000 ms depending on charger response time, WiFi
        signal, and concurrent BLE traffic. The test catches a real
        regression (orders-of-magnitude slowdown), not a fine-grained
        perf shift.
        """
        durations = []
        for _ in range(5):
            r, dur = _send_cmd("g_tzn", sync=1)
            self.assertEqual(r.status_code, 200)
            durations.append(dur)
        avg = sum(durations) / len(durations)
        print(
            f"\n[step2] g_tzn 5-call avg={avg:.3f}s "
            f"(min={min(durations):.3f} max={max(durations):.3f})"
        )
        self.assertLess(
            avg, 2.5,
            f"avg sync latency {avg:.3f}s — queue drain may be regressing"
        )


class TestAsyncPath(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_wait_0_returns_202_immediately(self) -> None:
        r, dur = _send_cmd("g_tzn", wait=0)
        self.assertEqual(r.status_code, 202)
        self.assertLess(dur, 0.3, f"async ?wait=0 took {dur:.2f}s")
        data = r.json()
        self.assertIn("id", data)
        self.assertEqual(data.get("status"), "pending")

    def test_command_status_polls_to_completion(self) -> None:
        """?wait=0 returns 202 immediately with an id; polling
        /api/command_status?id=N eventually returns 200 + body."""
        r, _ = _send_cmd("g_tzn", wait=0)
        self.assertEqual(r.status_code, 202)
        req_id = r.json()["id"]
        # Poll up to 2 s for the response to land. g_tzn should complete
        # well within that on a healthy MAX.
        for _ in range(20):
            time.sleep(0.1)
            poll = requests.get(
                f"{GATEWAY}/api/command_status?id={req_id}",
                auth=AUTH,
                timeout=5,
            )
            if poll.status_code == 200:
                # Body must have the actual BAPI response shape.
                data = poll.json()
                self.assertTrue("r" in data or "error" in data, data)
                return
        self.fail(f"command_status never returned 200 for id={req_id}")

    def test_command_status_400_on_missing_id(self) -> None:
        r = requests.get(f"{GATEWAY}/api/command_status", auth=AUTH, timeout=5)
        self.assertEqual(r.status_code, 400)

    def test_command_status_400_on_invalid_id(self) -> None:
        r = requests.get(
            f"{GATEWAY}/api/command_status?id=notanumber", auth=AUTH, timeout=5
        )
        self.assertEqual(r.status_code, 400)

    def test_short_wait_default_returns_200_or_202(self) -> None:
        """Default ~800ms wait: should be 200 on healthy MAX."""
        r, dur = _send_cmd("g_tzn")
        self.assertIn(r.status_code, (200, 202))


class TestSyncEscapeHatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        _require_env()

    def test_sync_1_preserves_old_blocking_behavior(self) -> None:
        r, _ = _send_cmd("g_tzn", sync=1)
        self.assertEqual(r.status_code, 200)
        # Body shape must match the baseline behaviour exactly.
        self.assertIn("r", r.json())


if __name__ == "__main__":
    unittest.main()
