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
static uint32_t _lastSample  = 0;   // epoch of the previous cp>0 sample
static double   _curWh       = 0.0; // power-integrated Wh so far this burst
// Green split: r_dat's en/gen are cumulative SESSION energy. Baselines at burst
// open + latest each sample give the burst's Δen/Δgen; the green FRACTION
// (Δgen/Δen) is scale-independent, so gwh = wh * fraction stays robust even if
// en/gen use a different unit scale than the power integral.
static uint32_t _en0  = 0, _gen0 = 0;   // cumulative en/gen at burst open
static uint32_t _enLast = 0, _genLast = 0;  // latest cumulative en/gen

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
    if (!p.begin(NVS_NS, false)) return;
    String s;
    serializeJson(doc, s);
    p.putString(NVS_KEY, s);
    p.end();
}

// ---- open-burst persistence (survive a reboot / OTA mid-charge) --------
// The open burst lives only in RAM, so a reboot mid-charge (or an r_dat feed
// stall that never delivers the cp-drop close-sample) would silently drop the
// whole charge. We persist it periodically and recover it on the next boot.
static const char* NVS_OPEN = "openb";
static const uint32_t PERSIST_INTERVAL_S = 300;  // re-persist at most this often (flash wear)
static const uint32_t STALE_TIMEOUT_S    = 600;  // no fresh cp sample this long -> close it
static uint32_t _lastPersist = 0;

static void saveOpenState() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    if (_open) {
        JsonDocument d;
        d["u"]  = _curUsid;
        d["st"] = _curStart;
        d["ls"] = _lastSample;
        d["wh"] = (uint32_t)(_curWh + 0.5);
        d["e0"] = _en0;  d["el"] = _enLast;
        d["g0"] = _gen0; d["gl"] = _genLast;
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
                uint32_t wh  = d["wh"].as<uint32_t>();
                uint32_t den  = d["el"].as<uint32_t>() > d["e0"].as<uint32_t>()
                                ? d["el"].as<uint32_t>() - d["e0"].as<uint32_t>() : 0;
                uint32_t dgen = d["gl"].as<uint32_t>() > d["g0"].as<uint32_t>()
                                ? d["gl"].as<uint32_t>() - d["g0"].as<uint32_t>() : 0;
                double frac = (den > 0) ? ((double)dgen / (double)den) : 0.0;
                if (frac > 1.0) frac = 1.0;
                uint32_t u = d["u"].as<uint32_t>();
                // Idempotent: closeBurst() appends the interval, THEN clears this
                // openb key (two NVS writes). If a crash landed between them, the
                // burst is already the newest stored interval — don't duplicate
                // it. (Match on usid+start.)
                bool already = arr.size() > 0
                    && (uint32_t)(arr[arr.size() - 1]["usid"] | 0) == u
                    && (uint32_t)(arr[arr.size() - 1]["start"] | 0) == st;
                if (wh > 0 && !already) {
                    appendInterval(u, st, stp, wh, (uint32_t)(wh * frac + 0.5));
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
    uint32_t wh = (uint32_t)(_curWh + 0.5);
    // Green Wh = power-integrated wh scaled by the burst's green fraction.
    uint32_t den  = (_enLast  > _en0)  ? (_enLast  - _en0)  : 0;
    uint32_t dgen = (_genLast > _gen0) ? (_genLast - _gen0) : 0;
    double frac = (den > 0) ? ((double)dgen / (double)den) : 0.0;
    if (frac > 1.0) frac = 1.0;
    uint32_t gwh = (uint32_t)(wh * frac + 0.5);
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

void onRealtime(const String& rdatJson) {
    if (rdatJson.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, rdatJson) != DeserializationError::Ok) return;
    JsonObjectConst r = doc["r"];
    if (r.isNull()) return;
    if (r["cp"].isNull()) return;                       // no power field

    const float    cp   = r["cp"].as<float>();          // charge power, kW (coerces num/str)
    const uint32_t usid = r["usid"].as<uint32_t>();     // 0 if absent
    const uint32_t en   = r["en"].as<uint32_t>();       // cumulative session energy
    const uint32_t gen  = r["gen"].as<uint32_t>();      // cumulative session green energy
    const uint32_t now  = (uint32_t)time(nullptr);
    if (now < EPOCH_VALID_MIN) return;                  // clock not synced yet

    const bool charging = (cp > CP_ON_KW);

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
            _en0 = _enLast = en;
            _gen0 = _genLast = gen;
            _chargingNow = true;
            _openSince   = now;
            _lastPersist = now;
            saveOpenState();   // persist immediately so an early reboot recovers it
            return;  // first sample of the burst — no energy yet
        }
        _enLast = en; _genLast = gen;
        // Integrate energy: Wh += kW * seconds / 3.6  (1 Wh = 3.6 kJ).
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
