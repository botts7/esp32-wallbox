#!/usr/bin/env python3
"""
probe_bapi.py — Wallbox BAPI compatibility audit.

Calls every read-only BAPI method the gateway uses and prints a structured
report of the response shape per method. Safe to run on a working gateway:
- ONLY hits getter / reader methods (g_*, r_*, read_pin).
- Does NOT call s_* (setters), w_sch (write schedule), clr_sch (clear),
  start/stop/lock/unlock/reboot, or anything that mutates charger state.
- Honors the gateway's rate limiter (token bucket, 4/burst, 2/s refill).
- Single-shot — runs each method once, in series, with a 1-second pause
  so the bucket never empties.

Why this exists:
  The Wallbox BAPI shape differs across charger models / firmware. The
  same `met` can return an integer on one model and a structured object
  on another (Auto Lock is one example: Pulsar MAX returns a plain
  number; newer firmware returns {enabled, time}). The web UI assumed
  one shape and broke on the other. Run this on each model you can
  reach, paste the output into a GitHub issue, and we can compare to
  build a model-compatibility map for the UI.

Stdlib only.

Usage:
  python scripts/probe_bapi.py --host wallbox-gw.local
  python scripts/probe_bapi.py --host 10.0.0.42 --label "Pulsar Plus FW 6.7.38"

Output: pretty-printed table + machine-readable JSON block at the end
(copy that block into a GitHub issue / DM).

Exit code 0 always (this is an audit, not a test).
"""

import argparse
import json
import sys
import time
import urllib.request
import urllib.error
from collections import OrderedDict

# Read-only BAPI methods that the wallbox-gateway web UI calls.
# Tuples: (method, human-readable name, what we expect to see).
SAFE_GETTERS = [
    ("g_alo",     "Auto Lock",          "object {enabled,time} OR number"),
    ("g_ocpp",    "OCPP",               "object {chid,e,pw,u}"),
    ("g_ecos",    "Eco Smart",          "object {ese,esm,esp}"),
    ("g_psh",    "Power Share",         "object {dyps,mcpp,minI,nchg}"),
    ("g_phsw",    "Phase Switch",       "object {enabled}"),
    ("g_halocfg", "Halo LED",           "object {bright,mode,time_s}"),
    ("g_tzn",     "Timezone",           "object {timezone}"),
    ("r_dca",     "Real-time meter",    "object with v1/p1 fields"),
    ("r_not",     "Notifications",      "array"),
    ("r_schs",    "Schedules",          "array OR {schedules:[]}"),
    ("read_pin",  "Bluetooth Passcode", "object {pin,version}"),
]


def http_get(host, path, timeout=12.0):
    url = "http://{}{}".format(host, path)
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
            return r.getcode(), body, time.monotonic() - t0
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace") if e.fp else ""
        return e.code, body, time.monotonic() - t0
    except Exception as e:
        return 0, "transport error: {}".format(e), time.monotonic() - t0


def shape_of(value):
    """Return a compact human description of the value's shape."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "number(int)"
    if isinstance(value, float):
        return "number(float)"
    if isinstance(value, str):
        return "string({} chars)".format(len(value))
    if isinstance(value, list):
        if not value:
            return "array(empty)"
        return "array[{}] of {}".format(len(value), shape_of(value[0]))
    if isinstance(value, dict):
        keys = sorted(value.keys())
        # Show the keys + per-key type so we can spot field-level mismatches.
        details = ",".join("{}:{}".format(k, shape_of(value[k])) for k in keys)
        return "object{{{}}}".format(details)
    return "unknown"


def probe(host, label, gateway_meta):
    print("=" * 78)
    print(" Wallbox BAPI compatibility probe")
    print("=" * 78)
    print(" target  : {}".format(host))
    if label:
        print(" label   : {}".format(label))
    if gateway_meta:
        print(" gateway : {}".format(gateway_meta))
    print(" methods : {} read-only getters".format(len(SAFE_GETTERS)))
    print(" pacing  : 1.0 s between calls (rate-limit friendly)")
    print()

    rows = []
    for met, name, expected in SAFE_GETTERS:
        path = "/api/command?action=bapi&met={}&par=null".format(met)
        code, body, elapsed = http_get(host, path)
        # Try to parse the BAPI response. The gateway wraps the BLE response
        # as {"id":N,"r":...} on success or {"error":"..."} on failure.
        try:
            parsed = json.loads(body)
        except Exception:
            parsed = None

        if parsed is None:
            shape = "(unparseable)"
            sample = body[:80]
        elif isinstance(parsed, dict) and "error" in parsed:
            shape = "error: " + str(parsed["error"])
            sample = ""
        elif isinstance(parsed, dict) and "r" in parsed:
            shape = shape_of(parsed["r"])
            sample = json.dumps(parsed["r"])[:120]
        else:
            shape = shape_of(parsed)
            sample = json.dumps(parsed)[:120]

        rows.append(OrderedDict([
            ("met", met),
            ("name", name),
            ("expected", expected),
            ("http", code),
            ("ms", round(elapsed * 1000)),
            ("shape", shape),
            ("sample", sample),
        ]))
        print("  {:10s} {:22s} http={:3d}  {:5d}ms  {}".format(
            met, name, code, round(elapsed * 1000), shape))
        if sample:
            print("  {:10s} {:22s}                     sample: {}".format(
                "", "", sample))
        time.sleep(1.0)

    print()
    print("=" * 78)
    print(" Copy the JSON below into a GitHub issue / share with maintainer:")
    print("=" * 78)
    print()
    out = OrderedDict([
        ("host", host),
        ("label", label or ""),
        ("gateway", gateway_meta or {}),
        ("results", rows),
    ])
    print(json.dumps(out, indent=2, ensure_ascii=False))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True, help="gateway hostname or IP (e.g. wallbox-gw.local)")
    ap.add_argument("--label", default="", help="human label for this charger, e.g. 'Pulsar Plus FW 6.7.38'")
    args = ap.parse_args()

    # Optional gateway metadata — version, charger model, FW — so the
    # paste-into-issue output self-describes the environment.
    code, body, _ = http_get(args.host, "/api/status", timeout=5.0)
    gateway_meta = {}
    if code == 200:
        try:
            s = json.loads(body)
            gateway_meta = {
                "dev_mfg":   s.get("dev_mfg"),
                "dev_model": s.get("dev_model"),
                "dev_fw":    s.get("dev_fw"),
                "dev_name":  s.get("dev_name"),
                "chg_sn":    s.get("chg_sn"),
                "ble":       s.get("ble"),
            }
        except Exception:
            pass

    probe(args.host, args.label, gateway_meta)


if __name__ == "__main__":
    main()
