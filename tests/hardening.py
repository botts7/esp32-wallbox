"""Hardening / soak / burst harness for the wallbox gateway.

Re-used per refactor step from 2.7.0 onwards. The goal is to prove
real-world-shape load against a LIVE charger without:

  * Sending any state-changing BAPI commands (no reboot, no
    start/stop, no lock — those would actually touch the user's
    charging session). Only g_tzn / r_dat reads.
  * Exceeding what HA would realistically generate. HA polls every
    30 s plus reacts to MQTT pushes. We cap our worst-case to ~3
    requests/sec which is well inside the token bucket (TB_CAP=4,
    refill=2/sec).
  * Triggering the OTA, settings-write, or session-write paths.

Failure modes the harness CATCHES:

  * BLE reconnect during load (would mean we starved keepalive or
    the BLE task panicked).
  * MQTT reconnect during load (would mean we starved the main
    loop's PubSubClient pump).
  * loop_max_ms regression past a threshold (proves main loop is
    no longer blocked by BAPI roundtrips after the async refactor).
  * Charger going to error status during load (would mean we
    actually broke the EVSE — game over).
  * Gateway becoming unresponsive (/api/health timeout).

Usage:
    python -m tests.hardening
"""

from __future__ import annotations

import os
import sys
import time
import threading
from dataclasses import dataclass, field
from typing import List, Optional

import requests


GATEWAY = os.environ.get("WB_GATEWAY", "").rstrip("/")
AUTH = (
    os.environ.get("WB_AUTH_USER", "admin"),
    os.environ.get("WB_AUTH_PASS", ""),
)


@dataclass
class HealthSnapshot:
    """One point-in-time view of /api/health for delta analysis."""

    uptime: int = 0
    ble_reconnects: int = 0
    mqtt_reconnects: int = 0
    loop_max_ms: int = 0
    tokens: int = 0
    ble_state: str = ""
    raw: dict = field(default_factory=dict)


def snapshot() -> HealthSnapshot:
    """Combined snapshot — uptime/loop_max_ms/tokens from /api/health
    (cheap, no auth check on most fields) + reconnect counters from
    /api/diag/disconnects (the authoritative wb_diag source)."""
    h = requests.get(f"{GATEWAY}/api/health", auth=AUTH, timeout=5)
    h.raise_for_status()
    hd = h.json()
    d = requests.get(f"{GATEWAY}/api/diag/disconnects", auth=AUTH, timeout=5)
    d.raise_for_status()
    dd = d.json()
    merged = {**hd, **dd}
    return HealthSnapshot(
        uptime=int(hd.get("uptime", 0)),
        ble_reconnects=int(dd.get("ble_reconnects", 0)),
        mqtt_reconnects=int(dd.get("mqtt_reconnects", 0)),
        loop_max_ms=int(hd.get("loop_max_ms", 0)),
        tokens=int(hd.get("tokens", 0)),
        ble_state="",  # not exposed on /api/health post-2.6.x — implicit
        raw=merged,
    )


@dataclass
class ChargerObservation:
    t: float
    ok: bool
    status_code: Optional[int]
    error: Optional[str] = None


class ChargerMonitor(threading.Thread):
    """Background thread that polls r_dat every interval seconds and
    records the BAPI status code (`st`). Used to prove the charger
    itself never trips into an error state under load.

    The charger's normal status codes are 0..14 (IDLE..CHARGING..etc).
    Anything that throws an error from the BAPI response is recorded
    and ends the run.
    """

    def __init__(self, interval: float = 3.0):
        super().__init__(daemon=True)
        self.interval = interval
        self._stop = threading.Event()
        self.observations: List[ChargerObservation] = []

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        while not self._stop.is_set():
            t = time.time()
            try:
                # Use ?sync=1 so we get the response inline; that's
                # the most stringent check (proves both the async
                # path and the sync escape hatch stay healthy).
                r = requests.get(
                    f"{GATEWAY}/api/command",
                    params={"action": "bapi", "met": "r_dat", "par": "null", "sync": "1"},
                    auth=AUTH,
                    timeout=8,
                )
                if r.status_code != 200:
                    self.observations.append(ChargerObservation(
                        t=t, ok=False, status_code=None,
                        error=f"http {r.status_code}: {r.text[:80]}"))
                else:
                    body = r.json()
                    if "error" in body:
                        self.observations.append(ChargerObservation(
                            t=t, ok=False, status_code=None,
                            error=str(body["error"])))
                    elif "r" in body and isinstance(body["r"], dict):
                        st = body["r"].get("st")
                        self.observations.append(ChargerObservation(
                            t=t, ok=True,
                            status_code=int(st) if st is not None else None))
                    else:
                        self.observations.append(ChargerObservation(
                            t=t, ok=False, status_code=None,
                            error=f"unexpected body shape: {body}"))
            except Exception as e:
                self.observations.append(ChargerObservation(
                    t=t, ok=False, status_code=None, error=str(e)))
            self._stop.wait(self.interval)


@dataclass
class BurstResult:
    sent: int
    ok: int
    status_2xx: int
    status_429: int
    status_503: int
    status_other: int
    durations: List[float]


def burst(
    n: int = 30,
    sync_every: int = 4,
    pace_seconds: float = 0.35,
    timeout: float = 10.0,
) -> BurstResult:
    """Realistic-shape burst: `n` read-only g_tzn calls, paced
    `pace_seconds` apart (default ~3 req/s), with every Nth call
    using `?sync=1` to exercise both paths. 429s are tolerated as
    expected token-bucket backpressure.
    """
    out = BurstResult(
        sent=0, ok=0, status_2xx=0, status_429=0,
        status_503=0, status_other=0, durations=[]
    )
    for i in range(n):
        params = {"action": "bapi", "met": "g_tzn", "par": "null"}
        if i % sync_every == 0:
            params["sync"] = "1"
        t0 = time.perf_counter()
        try:
            r = requests.get(
                f"{GATEWAY}/api/command",
                params=params, auth=AUTH, timeout=timeout,
            )
            dur = time.perf_counter() - t0
            out.durations.append(dur)
            out.sent += 1
            if 200 <= r.status_code < 300:
                out.status_2xx += 1
                out.ok += 1
            elif r.status_code == 429:
                out.status_429 += 1
            elif r.status_code == 503:
                out.status_503 += 1
            else:
                out.status_other += 1
        except Exception:
            out.sent += 1
            out.status_other += 1
        time.sleep(pace_seconds)
    return out


def run(soak_seconds: int = 60, burst_count: int = 30) -> int:
    """Full battery: snapshot → start charger monitor → burst load
    → wait soak → end snapshot → assert. Returns 0 on pass, 1 on
    fail. Exit code lets us chain into CI / shell."""

    if not GATEWAY or not AUTH[1]:
        print("ERROR: set WB_GATEWAY and WB_AUTH_PASS", file=sys.stderr)
        return 2

    print(f"[hardening] gateway={GATEWAY}")
    before = snapshot()
    print(
        f"[hardening] BEFORE: uptime={before.uptime}s "
        f"ble_reconnects={before.ble_reconnects} "
        f"mqtt_reconnects={before.mqtt_reconnects} "
        f"loop_max_ms={before.loop_max_ms} tokens={before.tokens}"
    )

    monitor = ChargerMonitor(interval=3.0)
    monitor.start()

    print(f"[hardening] burst: {burst_count} mixed sync+async @ ~3/s")
    br = burst(n=burst_count)
    print(
        f"[hardening] burst sent={br.sent} ok={br.ok} "
        f"2xx={br.status_2xx} 429={br.status_429} "
        f"503={br.status_503} other={br.status_other}"
    )
    if br.durations:
        avg = sum(br.durations) / len(br.durations)
        print(
            f"[hardening] burst latency avg={avg*1000:.0f}ms "
            f"min={min(br.durations)*1000:.0f}ms "
            f"max={max(br.durations)*1000:.0f}ms"
        )

    print(f"[hardening] soak for {soak_seconds}s while monitor runs")
    time.sleep(soak_seconds)

    monitor.stop()
    monitor.join(timeout=10)

    after = snapshot()
    print(
        f"[hardening] AFTER:  uptime={after.uptime}s "
        f"ble_reconnects={after.ble_reconnects} "
        f"mqtt_reconnects={after.mqtt_reconnects} "
        f"loop_max_ms={after.loop_max_ms} tokens={after.tokens}"
    )

    ok_obs = sum(1 for o in monitor.observations if o.ok)
    bad_obs = [o for o in monitor.observations if not o.ok]
    status_codes = sorted({o.status_code for o in monitor.observations if o.status_code is not None})
    print(
        f"[monitor] charger samples ok={ok_obs}/"
        f"{len(monitor.observations)} st_codes_seen={status_codes}"
    )

    # ---- Assertions ----
    fail: List[str] = []

    if after.ble_reconnects > before.ble_reconnects:
        fail.append(
            f"ble_reconnects bumped {before.ble_reconnects} -> "
            f"{after.ble_reconnects} during load"
        )
    if after.mqtt_reconnects > before.mqtt_reconnects:
        fail.append(
            f"mqtt_reconnects bumped {before.mqtt_reconnects} -> "
            f"{after.mqtt_reconnects} during load"
        )

    # Budget covers the sync escape hatch (?sync=1) which is allowed
    # to block the main task for up to ~waitMs (5000 ms, see
    # wb_web.cpp `handleApiCommand`). The async default path stays
    # under ~750 ms thanks to the chunked-wait pump introduced in
    # step 9c. The burst exercises 25% sync = at least one 5 s spike
    # is expected; 6500 ms gives headroom for OS-scheduling jitter
    # without masking real regressions.
    LOOP_BUDGET_MS = 6500
    if after.loop_max_ms > LOOP_BUDGET_MS:
        fail.append(
            f"loop_max_ms={after.loop_max_ms} exceeded budget "
            f"{LOOP_BUDGET_MS} ms"
        )

    if bad_obs:
        fail.append(
            f"charger monitor saw {len(bad_obs)} non-ok samples — "
            f"first: {bad_obs[0].error}"
        )
    if not monitor.observations:
        fail.append("charger monitor recorded zero samples")

    if br.status_503 > 0:
        fail.append(
            f"got {br.status_503} HTTP 503 — async queue should not "
            f"hit 503 under realistic-shape load"
        )

    if br.ok < int(br.sent * 0.85):
        fail.append(
            f"burst success rate {br.ok}/{br.sent} below 85% "
            f"(429 absorbed: {br.status_429})"
        )

    if fail:
        print("[hardening] FAIL:")
        for f in fail:
            print(f"  - {f}")
        return 1

    print("[hardening] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(run())
