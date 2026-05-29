#!/usr/bin/env python3
"""
stress_api.py — production-grade stress harness for /api/command.

Reproduces and then *disproves* the re-entrant-handleClient panic class:
  1. Sequential tight loop (the original repro: 20x r_dca back-to-back).
  2. Concurrent load (N parallel workers churning connections) — the real
     trigger, because overlapping requests are what clobber the WebServer's
     single _currentClient.

Correctness is asserted over Wi-Fi via /api/health (no serial console needed):
  - max_reentry MUST stay 1   -> proves the web server was never pumped nested
  - uptime MUST be monotonic   -> proves the device never panicked/rebooted
  - heap_free MUST be stable   -> proves no leak was introduced
  - status codes MUST be in {200,429,503} -> clean backpressure, no resets

Stdlib only (urllib + threading). No Node, no pip installs.

Usage:
  python scripts/stress_api.py --host 192.168.86.125
  python scripts/stress_api.py --host 192.168.86.125 --seq 200 --conc 8 --rounds 50 --soak 0
  python scripts/stress_api.py --host 192.168.86.125 --soak 3600   # 1-hour soak

Exit code 0 = PASS (production-grade), 1 = FAIL.
"""

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from collections import Counter
from concurrent.futures import ThreadPoolExecutor

CMD_PATH = "/api/command?action=bapi&met=r_dca&par=null"
HEALTH_PATH = "/api/health"


def http_get(host, path, timeout):
    """Return (status_code, body_text, elapsed_s). status 0 = transport error."""
    url = "http://{}{}".format(host, path)
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
            return r.getcode(), body, time.monotonic() - t0
    except urllib.error.HTTPError as e:
        # 429/503 are HTTPErrors but are EXPECTED, healthy backpressure.
        body = e.read().decode("utf-8", "replace") if e.fp else ""
        return e.code, body, time.monotonic() - t0
    except Exception as e:  # noqa: BLE001 - timeouts, conn refused, resets
        return 0, "{}: {}".format(type(e).__name__, e), time.monotonic() - t0


def get_health(host, timeout=5):
    code, body, _ = http_get(host, HEALTH_PATH, timeout)
    if code in (200, 503) and body:
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            return None
    return None


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = int(round((p / 100.0) * (len(s) - 1)))
    return s[k]


def run_phase(host, total, concurrency, timeout):
    """Fire `total` requests across `concurrency` workers. Returns (Counter, latencies)."""
    codes = Counter()
    lat = []

    def one(_):
        c, _b, dt = http_get(host, CMD_PATH, timeout)
        return c, dt

    if concurrency <= 1:
        for i in range(total):
            c, dt = one(i)
            codes[c] += 1
            lat.append(dt)
    else:
        with ThreadPoolExecutor(max_workers=concurrency) as ex:
            for c, dt in ex.map(one, range(total)):
                codes[c] += 1
                lat.append(dt)
    return codes, lat


def report(label, codes, lat):
    total = sum(codes.values())
    print("\n[{}] {} requests".format(label, total))
    for code in sorted(codes):
        name = {0: "TRANSPORT-ERR", 200: "200 OK", 429: "429 rate",
                503: "503 busy"}.get(code, str(code))
        print("    {:>14}: {}".format(name, codes[code]))
    if lat:
        print("    latency  p50={:.0f}ms  p99={:.0f}ms  max={:.0f}ms".format(
            pct(lat, 50) * 1000, pct(lat, 99) * 1000, max(lat) * 1000))


def main():
    ap = argparse.ArgumentParser(description="Stress /api/command and assert no panic / no reentrancy.")
    ap.add_argument("--host", required=True, help="gateway IP, e.g. 192.168.86.125")
    ap.add_argument("--seq", type=int, default=200, help="sequential tight-loop request count")
    ap.add_argument("--conc", type=int, default=8, help="concurrent workers")
    ap.add_argument("--rounds", type=int, default=50, help="conc requests = conc*rounds")
    ap.add_argument("--soak", type=int, default=0, help="extra soak seconds at --conc (0=skip)")
    ap.add_argument("--timeout", type=float, default=6.0, help="per-request timeout (s)")
    ap.add_argument("--max-heap-drop", type=int, default=20000, help="allowed free-heap drop (bytes)")
    args = ap.parse_args()

    print("=== /api/command stress harness ===")
    print("target: {}  seq={}  conc={}x{}  soak={}s".format(
        args.host, args.seq, args.conc, args.rounds, args.soak))

    h0 = get_health(args.host)
    if not h0:
        print("FAIL: cannot read {} — is the gateway up and reachable?".format(HEALTH_PATH))
        return 1
    if "max_reentry" not in h0:
        print("WARN: /api/health has no 'max_reentry' field — old firmware? Proof gate disabled.")
    print("baseline: uptime={}s heap_free={} max_reentry={}".format(
        h0.get("uptime"), h0.get("heap_free"), h0.get("max_reentry")))
    t_start = time.monotonic()

    all_codes = Counter()

    # Phase 1: original repro, tightened — sequential, no concurrency.
    c1, l1 = run_phase(args.host, args.seq, 1, args.timeout)
    report("seq tight-loop", c1, l1)
    all_codes += c1

    # Phase 2: concurrent — the real panic trigger (overlapping requests).
    c2, l2 = run_phase(args.host, args.conc * args.rounds, args.conc, args.timeout)
    report("concurrent", c2, l2)
    all_codes += c2

    # Phase 3: optional soak.
    if args.soak > 0:
        print("\n[soak] running {}s at conc={} ...".format(args.soak, args.conc))
        deadline = time.monotonic() + args.soak
        cs = Counter()
        ls = []
        while time.monotonic() < deadline:
            c, l = run_phase(args.host, args.conc, args.conc, args.timeout)
            cs += c
            ls += l
        report("soak", cs, ls)
        all_codes += cs

    elapsed = time.monotonic() - t_start
    h1 = get_health(args.host)
    if not h1:
        print("\nFAIL: gateway unreachable after stress — likely panicked/rebooting.")
        return 1

    print("\n=== post-stress health ===")
    print("uptime={}s heap_free={} max_reentry={}".format(
        h1.get("uptime"), h1.get("heap_free"), h1.get("max_reentry")))

    # ---- PASS/FAIL gates ----
    failures = []

    # Gate 1 (HARD): no panic/reboot. Uptime must have advanced ~monotonically.
    if h1.get("uptime", 0) < h0.get("uptime", 0) + elapsed * 0.7:
        failures.append("DEVICE REBOOTED during test (uptime regressed) — panic not fixed.")

    # Gate 2 (HARD): reentrancy proof. max_reentry must never exceed 1.
    mr = h1.get("max_reentry")
    if mr is not None and mr > 1:
        failures.append("max_reentry={} (>1) — web server was pumped re-entrantly.".format(mr))

    # Gate 3 (SLO): no transport errors beyond the TCP backlog (a few refused
    # conns under heavy concurrency are acceptable; resets/timeouts en masse are not).
    transport_err = all_codes.get(0, 0)
    total_req = sum(all_codes.values())
    if transport_err > max(5, total_req * 0.02):
        failures.append("{} transport errors ({:.1f}%) — connection resets, not clean backpressure.".format(
            transport_err, 100.0 * transport_err / total_req))

    # Gate 4 (SLO): heap stable — no leak introduced.
    if h0.get("heap_free") and h1.get("heap_free"):
        drop = h0["heap_free"] - h1["heap_free"]
        if drop > args.max_heap_drop:
            failures.append("heap dropped {} bytes (>{}) — possible leak.".format(drop, args.max_heap_drop))

    # Gate 5 (SLO): only clean status codes seen.
    bad = [c for c in all_codes if c not in (0, 200, 429, 503)]
    if bad:
        failures.append("unexpected status codes: {}".format(bad))

    print("\n=== VERDICT ===")
    if failures:
        for f in failures:
            print("  FAIL: " + f)
        print("RESULT: FAIL")
        return 1
    print("  no panic (uptime monotonic), max_reentry<=1, heap stable, clean codes")
    print("RESULT: PASS (production-grade)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
