#include "wb_charge_log.h"
#include "wb_log.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

namespace wb_charge_log {

static const char* NVS_NS  = "wbcharge";
static const char* NVS_KEY = "ivals";

// SNTP-synced guard: before this the clock is the 1970 boot default, so any
// timestamp would be garbage. Matches the wb_ble / wb_health convention.
static const uint32_t EPOCH_VALID_MIN = 1700000000;  // ~2023-11

// ---- open-burst state (owned by the single realtime-drain task) ----
static bool     _open        = false;
static uint32_t _curUsid     = 0;
static uint32_t _curStart    = 0;   // burst start epoch (UTC)
static uint32_t _lastSample  = 0;   // epoch of the previous charging sample
static double   _curWh       = 0.0; // cp power-integral Wh (fallback energy)
static bool     _haveEn      = false; // this burst carries metered r_dat.en
static uint32_t _en0         = 0;   // r_dat.en (centi-kWh) at burst open
static uint32_t _enLast      = 0;   // latest r_dat.en
static double   _grn0        = 0.0; // r_lse session green (kWh) at burst open

// en-rising detector — remembers the last en per session so a cp≈0 burst
// (Eco-Smart solar, where cp reads ~0 while energy is delivered) is still
// caught when metered energy climbs. Kept across open/closed state.
static uint32_t _enTrackUsid = 0;
static uint32_t _enTrackVal  = 0;

// Latest authoritative per-session GREEN energy (kWh) from r_lse, fed by
// onLseGreen(). This — not r_dat.gen (the sticky override flag) — is the source
// for a burst's green (solar) Wh.
static volatile double _lseGreenKwh = 0.0;

// ---- live summary (best-effort cross-task reads) ----
static volatile bool     _chargingNow = false;
static volatile uint32_t _openSince   = 0;
static volatile uint8_t  _count       = 0;
static volatile uint32_t _lastBurstWh = 0;

// ---- NVS-backed ring (JSON array string in one key, like wb_diag) ----

static void load(JsonDocument& doc) {
    Preferences p;
    if (!p.begin(NVS_NS, true)) { doc.to<JsonArray>(); return; }
    String s = p.getString(NVS_KEY, "[]");
    p.end();
    if (deserializeJson(doc, s) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        doc.clear();
        doc.to<JsonArray>();
    }
}

static void store(const JsonDocument& doc) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) {
        Log.println("[chargelog] NVS begin(rw) failed — interval NOT persisted");
        return;
    }
    String s;
    serializeJson(doc, s);
    // Free the old value BEFORE writing the new one. Growing a key in place needs
    // room for old+new simultaneously; on a near-full NVS partition that write
    // silently fails and the ring gets stranded at its last-fitting size (this is
    // why the log sat at a fixed count while bursts kept "closing"). remove()
    // first frees the space so the write only needs room for the new blob.
    p.remove(NVS_KEY);
    size_t wrote = p.putString(NVS_KEY, s);
    p.end();
    if (wrote == 0 && s.length() > 0)
        Log.printf("[chargelog] NVS putString failed (%u bytes, partition full?) "
                   "— interval NOT persisted\n", (unsigned)s.length());
}

// ---- open-burst persistence (survive a reboot / OTA mid-charge) --------
// The open burst lives only in RAM, so a reboot mid-charge (or an r_dat feed
// stall that never delivers the cp-drop close-sample) would silently drop the
// whole charge. We persist it periodically and recover it on the next boot.
static const char* NVS_OPEN = "openb";
static const uint32_t PERSIST_INTERVAL_S = 300;  // re-persist at most this often (flash wear)
static const uint32_t STALE_TIMEOUT_S    = 600;  // no fresh cp sample this long -> close it
static uint32_t _lastPersist = 0;

// Energy (Wh) captured so far this burst: prefer the METERED delta from
// r_dat.en (centi-kWh → ×10 Wh) which is the charger's own accounting; fall back
// to the cp time-integral for models that don't report en (Zentri — synthesized
// cp only). Both are exact-enough; the metered path also survives cp≈0 solar.
static uint32_t _burstWh() {
    if (_haveEn && _enLast > _en0) return (uint32_t)((_enLast - _en0) * 10);
    return (uint32_t)(_curWh + 0.5);
}

// Green (solar) Wh this burst from the authoritative r_lse session-green delta.
// Clamped to [0, wh]. 0 when r_lse was never fed (e.g. Zentri) → treated as grid.
static uint32_t _burstGreenWh(uint32_t wh) {
    double dg = (double)_lseGreenKwh - _grn0;
    if (dg <= 0.0) return 0;
    uint32_t g = (uint32_t)(dg * 1000.0 + 0.5);
    return (g > wh) ? wh : g;
}

static void saveOpenState() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    if (_open) {
        uint32_t wh = _burstWh();
        JsonDocument d;
        d["u"]   = _curUsid;
        d["st"]  = _curStart;
        d["ls"]  = _lastSample;
        d["wh"]  = wh;                    // metered-or-integrated Wh so far
        d["gwh"] = _burstGreenWh(wh);     // authoritative green Wh so far
        String s;
        serializeJson(d, s);
        p.putString(NVS_OPEN, s);
    } else {
        p.remove(NVS_OPEN);
    }
    p.end();
}

static void appendInterval(uint32_t usid, uint32_t start, uint32_t stop,
                           uint32_t wh, uint32_t gwh) {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();
    JsonObject e = arr.add<JsonObject>();
    e["usid"]  = usid;
    e["start"] = start;
    e["stop"]  = stop;
    e["wh"]    = wh;
    e["gwh"]   = gwh;   // green (solar) Wh within this burst
    while ((int)arr.size() > MAX_INTERVALS) arr.remove(0);
    store(doc);
    _count = (uint8_t)arr.size();
}

// ---- public ----

void begin() {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();
    _count = (uint8_t)arr.size();
    // Restore last_burst_wh from the newest stored interval so the HA entity /
    // dashboard row shows a real value after a reboot, not 0-until-next-charge.
    if (arr.size() > 0) _lastBurstWh = (uint32_t)(arr[arr.size() - 1]["wh"] | 0);
    Log.printf("[chargelog] loaded %u stored charge intervals (last %uWh)\n",
               (unsigned)_count, (unsigned)_lastBurstWh);

    // Recover a burst that a reboot / OTA interrupted mid-charge. It was
    // persisted periodically (saveOpenState); append what we captured as a
    // completed interval so the charge isn't lost, then clear the pending state.
    Preferences po;
    if (po.begin(NVS_NS, true)) {
        String s = po.getString(NVS_OPEN, "");
        po.end();
        if (s.length()) {
            JsonDocument d;
            if (deserializeJson(d, s) == DeserializationError::Ok
                    && d["st"].as<uint32_t>() > EPOCH_VALID_MIN) {
                uint32_t st  = d["st"].as<uint32_t>();
                uint32_t stp = d["ls"].as<uint32_t>();      // last good sample = stop
                if (stp < st) stp = st;
                uint32_t wh  = d["wh"].as<uint32_t>();       // persisted computed Wh
                uint32_t gwh = d["gwh"].as<uint32_t>();      // persisted green Wh
                if (gwh > wh) gwh = wh;
                uint32_t u = d["u"].as<uint32_t>();
                // Idempotent: closeBurst() appends the interval, THEN clears this
                // openb key (two NVS writes). If a crash landed between them, the
                // burst is already the newest stored interval — don't duplicate
                // it. (Match on usid+start.)
                bool already = arr.size() > 0
                    && (uint32_t)(arr[arr.size() - 1]["usid"] | 0) == u
                    && (uint32_t)(arr[arr.size() - 1]["start"] | 0) == st;
                if (wh > 0 && !already) {
                    appendInterval(u, st, stp, wh, gwh);
                    _lastBurstWh = wh;
                    Log.printf("[chargelog] recovered interrupted burst: usid=%u "
                               "%us..%us %uWh\n", (unsigned)u,
                               (unsigned)st, (unsigned)stp, (unsigned)wh);
                }
            }
            // Clear it (recovered or invalid) so it can't re-append next boot.
            Preferences pw;
            if (pw.begin(NVS_NS, false)) { pw.remove(NVS_OPEN); pw.end(); }
        }
    }
}

static void closeBurst(uint32_t stop) {
    if (!_open) return;
    // Guard against a clock jump making stop < start.
    if (stop < _curStart) stop = _curStart;
    uint32_t wh  = _burstWh();
    uint32_t gwh = _burstGreenWh(wh);
    appendInterval(_curUsid, _curStart, stop, wh, gwh);
    _lastBurstWh = wh;
    Log.printf("[chargelog] burst closed: usid=%u %us..%us %uWh (%uWh green)\n",
               (unsigned)_curUsid, (unsigned)_curStart, (unsigned)stop,
               (unsigned)wh, (unsigned)gwh);
    _open = false;
    _chargingNow = false;
    _openSince = 0;
    _curWh = 0.0;
    saveOpenState();   // burst closed → clear the persisted pending state
}

void onLseGreen(double sessionGreenKwh) {
    // r_lse's cumulative session green energy (kWh). Non-negative guard only —
    // baselining/deltaing happens against the open burst in _burstGreenWh().
    if (sessionGreenKwh >= 0.0) _lseGreenKwh = sessionGreenKwh;
}

void onRealtime(const String& rdatJson) {
    if (rdatJson.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, rdatJson) != DeserializationError::Ok) return;
    JsonObjectConst r = doc["r"];
    if (r.isNull()) return;

    const bool hasCp = !r["cp"].isNull();
    const bool hasEn = !r["en"].isNull();
    if (!hasCp && !hasEn) return;                       // nothing usable this sample

    const float    cp   = hasCp ? r["cp"].as<float>() : 0.0f;  // charge power, kW
    const uint32_t usid = r["usid"].as<uint32_t>();     // 0 if absent
    const uint32_t en   = hasEn ? r["en"].as<uint32_t>() : 0;  // session energy, centi-kWh
    const uint32_t now  = (uint32_t)time(nullptr);
    if (now < EPOCH_VALID_MIN) return;                  // clock not synced yet

    // en-rising detection: metered session energy climbed since the last sample
    // of the SAME session. Catches Eco-Smart solar where cp reads ~0. One-sample
    // latency; the cp>CP_ON path catches the normal case immediately. The
    // per-session baseline reset stops a usid change from looking like a jump.
    bool enRising = false;
    if (hasEn) {
        if (usid == _enTrackUsid && en > _enTrackVal) enRising = true;
        _enTrackUsid = usid;
        _enTrackVal  = en;
    }

    const bool charging = (cp > CP_ON_KW) || enRising;

    if (charging) {
        // A new session id while a burst is open means the old session ended
        // between samples — close it before opening the new one.
        if (_open && usid != 0 && usid != _curUsid) closeBurst(now);

        if (!_open) {
            _open        = true;
            _curUsid     = usid;
            _curStart    = now;
            _lastSample  = now;
            _curWh       = 0.0;
            _haveEn      = hasEn;
            _en0 = _enLast = en;
            _grn0        = _lseGreenKwh;   // baseline authoritative green at open
            _chargingNow = true;
            _openSince   = now;
            _lastPersist = now;
            saveOpenState();   // persist immediately so an early reboot recovers it
            return;  // first sample of the burst — no energy yet
        }
        if (hasEn) { _haveEn = true; _enLast = en; }
        // cp time-integral (fallback energy for models without en): Wh += kW*s/3.6.
        uint32_t dt = (now > _lastSample) ? (now - _lastSample) : 0;
        if (dt > 0 && dt < 3600) {  // ignore absurd gaps (reboot/clock jump)
            _curWh += (double)cp * (double)dt / 3.6;
        }
        _lastSample = now;
    } else if (_open) {
        closeBurst(now);
    }
}

// Called periodically (independent of the seq-gated realtime feed) so an open
// burst is never lost when r_dat stops flowing, and is re-persisted for reboot
// recovery. Safe to call every loop — it self-throttles.
void tick(uint32_t now) {
    if (!_open || now < EPOCH_VALID_MIN) return;
    // r_dat feed stalled (BLE hiccup) — the cp-drop close-sample will never
    // arrive. Close at the last good sample so the charge is captured, not lost.
    if (_lastSample && now > _lastSample + STALE_TIMEOUT_S) {
        Log.printf("[chargelog] stale open burst — no cp sample for %us, closing\n",
                   (unsigned)(now - _lastSample));
        closeBurst(_lastSample);
        return;
    }
    if (now > _lastPersist + PERSIST_INTERVAL_S) {
        _lastPersist = now;
        saveOpenState();
    }
}

String toJson() {
    JsonDocument doc;
    load(doc);
    JsonArray src = doc.as<JsonArray>();

    JsonDocument out;
    out["charging_now"] = _chargingNow;
    out["open_since"]   = _openSince;
    out["count"]        = (uint32_t)src.size();
    JsonArray arr = out["intervals"].to<JsonArray>();
    // Newest first.
    for (int i = (int)src.size() - 1; i >= 0; i--) arr.add(src[i]);

    String s;
    serializeJson(out, s);
    return s;
}

bool     chargingNow()    { return _chargingNow; }
uint32_t openSinceEpoch() { return _openSince; }
uint8_t  count()          { return _count; }
uint32_t lastBurstWh()    { return _lastBurstWh; }

void clear() {
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.remove(NVS_KEY); p.end(); }
    _count = 0;
    _lastBurstWh = 0;
    // Leave any open burst running — clear() only wipes stored history.
}

}  // namespace wb_charge_log
