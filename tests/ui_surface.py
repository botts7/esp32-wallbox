"""UI surface probe — mimics every read-path BAPI call the GUI makes.

After the 2.7.0 async refactor, default /api/command behavior
changed and the GUI's `d.r` contract assumption broke for several
slow BAPI calls. This harness enumerates every BAPI read site the
JS in wb_web.cpp fires, calls them the same way the GUI does, and
asserts the response shape the GUI's `.then(...)` callbacks expect.

Only READ-path BAPI methods (g_*, r_*, read_*). NO `s_*` / `w_*` /
`clr_*` writes. We do not want to mutate the user's charger config
during validation.

Run:
    python -m tests.ui_surface

Failures are tabulated at the end with: method, what the JS expects,
what we actually saw, suggested fix (longer ?wait, ?sync, or
JS-side handling).
"""

from __future__ import annotations

import os
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, List, Optional

import requests


GATEWAY = os.environ.get("WB_GATEWAY", "").rstrip("/")
AUTH = (
    os.environ.get("WB_AUTH_USER", "admin"),
    os.environ.get("WB_AUTH_PASS", ""),
)


@dataclass
class Probe:
    """One UI-fetch site to validate.

    Each probe corresponds to a `fetch('/api/command?...')` in
    wb_web.cpp. The validator captures what the JS callback expects
    (e.g. `d.r.timezone` for the timezone fetch) so we catch shape
    drift, not just HTTP status.
    """
    name: str
    met: str
    par: str = "null"
    # Extra query params (e.g. {"sync": "1"} or {"wait": "10000"}).
    # Empty dict means "exact GUI default."
    extra: dict = field(default_factory=dict)
    # Validator returns None on success, str on failure (the
    # human-readable reason). Receives the parsed JSON `d`.
    validator: Callable[[dict], Optional[str]] = lambda d: None
    # Client-side timeout in seconds. The GUI uses AbortSignal.timeout
    # at 8-15 s depending on site — we mirror.
    timeout: float = 12.0
    # Whether the GUI's catch handler treats a non-d.r response as
    # "Couldn't read X state" (most setup modals) vs a hard error
    # (toast/Diag). Not tested directly but documents intent.
    gui_error_path: str = ""


@dataclass
class ProbeResult:
    name: str
    ok: bool
    http_status: int
    dur_seconds: float
    reason: str = ""


# ----- Validators ------------------------------------------------------------

def _expect_r_object(d: dict) -> Optional[str]:
    """Most setup modals do `if(!d.r||typeof d.r!=='object')`."""
    if "r" not in d:
        return "no `r` field"
    if not isinstance(d["r"], (dict, list)):
        return f"r is type {type(d['r']).__name__}, expected object/array"
    return None


def _expect_r_or_simple(d: dict) -> Optional[str]:
    """g_alo (Auto Lock) accepts either {r: number} OR {r: {enabled, time}}."""
    if "r" not in d:
        return "no `r` field"
    if not isinstance(d["r"], (dict, int, float)):
        return f"r is type {type(d['r']).__name__}, expected object or number"
    return None


def _expect_r_with_keys(*keys):
    def v(d):
        miss = _expect_r_object(d)
        if miss:
            return miss
        r = d["r"]
        if isinstance(r, dict):
            missing = [k for k in keys if k not in r]
            if missing:
                return f"r missing keys: {missing}"
        return None
    return v


def _expect_r_field(field_name):
    def v(d):
        miss = _expect_r_object(d)
        if miss:
            return miss
        r = d["r"]
        if isinstance(r, dict) and field_name not in r:
            return f"r missing `{field_name}`"
        return None
    return v


def _accept_any_body(d):
    """The /info Bench tool just prints whatever comes back. Any
    JSON shape is acceptable — even an error in body."""
    return None


# ----- The probe catalogue ---------------------------------------------------

# Order matters slightly — we put cheap calls first so a 503 cascade
# doesn't poison the whole run.
PROBES: List[Probe] = [
    Probe("home_tzn_preload",      "g_tzn",     validator=_expect_r_field("timezone"), timeout=8),
    Probe("home_pin_check",        "read_pin",  validator=_expect_r_object,            timeout=8),
    Probe("info_tzn_preload",      "g_tzn",     validator=_expect_r_field("timezone"), timeout=8),
    Probe("modal_autolock_load",   "g_alo",     validator=_expect_r_or_simple,         timeout=10),
    Probe("modal_ocpp_load",       "g_ocpp",    validator=_expect_r_object,            timeout=12),
    Probe("modal_eco_load",        "g_ecos",    validator=_expect_r_object,            timeout=12),
    Probe("modal_phaseswitch_load","g_phsw",    validator=_expect_r_object,            timeout=10),
    Probe("modal_halo_load",       "g_halocfg", validator=_expect_r_object,            timeout=10),
    Probe("modal_tz_load",         "g_tzn",     validator=_expect_r_field("timezone"), timeout=10),
    Probe("dashboard_meter_pull",  "r_dca",     validator=_expect_r_object,            timeout=10),
    Probe("dashboard_status_pull", "r_dat",     validator=_expect_r_with_keys("st"),   timeout=10),
    Probe("info_card_notifications","r_not",    validator=_accept_any_body,            timeout=10),
    Probe("info_card_wifi_status", "gwsta",     validator=_accept_any_body,            timeout=10),
    Probe("info_card_network",     "gnsta",     validator=_accept_any_body,            timeout=10),
    Probe("info_card_fwupdate",    "gupdc",     validator=_accept_any_body,            timeout=10),
    Probe("info_card_ocpp",        "g_ocpp",    validator=_expect_r_object,            timeout=12),
    Probe("sessions_recent_list",  "r_ses",     validator=_accept_any_body,            timeout=15),
    Probe("schedules_list",        "r_schs",    validator=_accept_any_body,            timeout=15),
]


def call(probe: Probe) -> ProbeResult:
    params = {"action": "bapi", "met": probe.met, "par": probe.par}
    params.update(probe.extra)
    t0 = time.perf_counter()
    try:
        r = requests.get(
            f"{GATEWAY}/api/command",
            params=params, auth=AUTH, timeout=probe.timeout,
        )
    except requests.Timeout:
        return ProbeResult(
            name=probe.name, ok=False, http_status=0,
            dur_seconds=time.perf_counter() - t0,
            reason=f"client timeout after {probe.timeout}s",
        )
    except Exception as e:
        return ProbeResult(
            name=probe.name, ok=False, http_status=0,
            dur_seconds=time.perf_counter() - t0,
            reason=f"connection error: {e}",
        )
    dur = time.perf_counter() - t0
    if r.status_code == 429:
        # Token bucket — treat as transient, not a hard fail.
        return ProbeResult(
            name=probe.name, ok=False, http_status=429, dur_seconds=dur,
            reason="rate-limited (transient — retry would clear)",
        )
    if r.status_code == 202:
        # The whole point of this harness: 202 means the GUI gets
        # `{"id":N, "status":"pending"}` instead of `d.r`. That's
        # exactly what breaks setup modals — flag as fail.
        return ProbeResult(
            name=probe.name, ok=False, http_status=202, dur_seconds=dur,
            reason="202 pending — GUI .then() will see {id, status} not d.r",
        )
    if r.status_code != 200:
        return ProbeResult(
            name=probe.name, ok=False, http_status=r.status_code,
            dur_seconds=dur,
            reason=f"http {r.status_code}: {r.text[:120]}",
        )
    try:
        d = r.json()
    except Exception:
        return ProbeResult(
            name=probe.name, ok=False, http_status=200, dur_seconds=dur,
            reason=f"non-JSON body: {r.text[:120]}",
        )
    if "error" in d:
        return ProbeResult(
            name=probe.name, ok=False, http_status=200, dur_seconds=dur,
            reason=f"error in body: {d['error']}",
        )
    reason = probe.validator(d)
    if reason:
        return ProbeResult(
            name=probe.name, ok=False, http_status=200, dur_seconds=dur,
            reason=f"shape: {reason} | body={str(d)[:120]}",
        )
    return ProbeResult(
        name=probe.name, ok=True, http_status=200, dur_seconds=dur,
        reason="",
    )


def run() -> int:
    if not GATEWAY or not AUTH[1]:
        print("ERROR: set WB_GATEWAY and WB_AUTH_PASS", file=sys.stderr)
        return 2

    print(f"[ui-surface] gateway={GATEWAY}, probes={len(PROBES)}")
    print(f"[ui-surface] pacing 0.5s between probes (token bucket safe)")

    results: List[ProbeResult] = []
    for p in PROBES:
        res = call(p)
        mark = "OK " if res.ok else "FAIL"
        print(
            f"  [{mark}] {p.name:30s} met={p.met:10s} "
            f"http={res.http_status:3d} dur={res.dur_seconds*1000:5.0f}ms"
            f"  {res.reason}"
        )
        results.append(res)
        time.sleep(0.5)

    fails = [r for r in results if not r.ok]
    print()
    print(f"[ui-surface] {len(results) - len(fails)}/{len(results)} probes ok")
    if fails:
        print(f"[ui-surface] FAIL: {len(fails)} probes")
        for r in fails:
            print(f"  - {r.name}: {r.reason}")
        return 1
    print("[ui-surface] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(run())
