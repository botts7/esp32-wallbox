#pragma once

#include <Arduino.h>

// Per-session charge-interval tracker.
//
// r_log (the charger's session log) only records, for each finished session,
// the plug-in time, the unplug time, and the *total* charging seconds — never
// WHEN within that span the charging actually happened. So a car plugged in at
// 7pm that charges 12am-6am on a schedule looks, from r_log alone, like it
// charged from 7pm. That makes every time-of-use cost/heatmap wrong.
//
// This module recovers the real windows by watching the live charge power
// (BAPI r_dat field `cp`, kW) AND the metered session energy (r_dat.en) on each
// realtime sample, recording every charging *burst* (a contiguous charging
// period) as an interval. A session can have many intervals (pause/resume,
// dynamic load, solar throttle); a night with no charging has none. It captures
// manual / unscheduled charging exactly as well as scheduled.
//
// Detection is cp>CP_ON OR en-rising: some chargers/modes report cp≈0 while still
// delivering energy (e.g. Eco-Smart *solar* on a Pulsar MAX), so gating on cp
// alone silently dropped those bursts. en (centi-kWh) rising within a session is
// the model-safe fallback. Energy per burst is the metered Δen when available,
// else the cp time-integral (Zentri, which has no en — only a synthesized cp).
//
// Intervals are mirrored to NVS (like wb_diag) so reboots don't wipe history.
// The in-progress (open) burst is also persisted periodically so a reboot / OTA
// mid-charge recovers it on the next boot instead of silently dropping the whole
// charge. Write churn is bounded — per burst: one write on open, one per
// ~PERSIST_INTERVAL while charging, one on close. Nothing per realtime sample.
// Forward-only: it cannot reconstruct sessions that finished before this ran.

namespace wb_charge_log {

// Ring size. Each interval is a small (~60 B) JSON record, persisted in the
// dedicated nvs2 partition (64 KB) so it no longer crowds settings in the 20 KB
// default nvs (see the migration in .cpp — that crowding was the #166 silent-
// fail). The classic ESP32-WROOM keeps a shorter ring to conserve RAM; the S3
// holds a longer history so month cost/savings (#151) doesn't undercount a busy
// month (a 24-burst ring can drop older bursts within a single month).
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static const uint8_t MAX_INTERVALS = 96;
#else
static const uint8_t MAX_INTERVALS = 24;
#endif

// Charging is "on" when cp exceeds this (kW) — filters meter noise / standby.
static constexpr float CP_ON_KW = 0.10f;

// Initialise (loads the persisted ring count). Call once after NVS + NTP.
void begin();

// Feed every new realtime r_dat JSON sample (the full {"r":{...}} response).
// Detects burst rising/falling edges and records closed bursts. Cheap; safe
// to call on every realtime poll. Must be called from a single task (the main
// task's realtime drain) — it owns the open-burst state.
void onRealtime(const String& rdatJson);

// Feed the latest r_lse sample's cumulative session GREEN energy (kWh, already
// scaled). Used to attribute the open burst's green (solar) Wh from the
// authoritative per-session figure — r_dat.gen is the schedule/eco override flag
// on MAX/Plus, NOT green energy. No-op if never fed (green falls back to 0, i.e.
// the burst is treated as all-grid). Call from the same task as onRealtime.
void onLseGreen(double sessionGreenKwh);

// Periodic housekeeping — call from the main loop (self-throttles). Closes an
// open burst whose r_dat feed has stalled (STALE_TIMEOUT) so it isn't lost, and
// re-persists the open burst for reboot recovery. `now` = epoch seconds.
void tick(uint32_t now);

// Serialized JSON for /api/charge_log:
//   {"charging_now":bool,"open_since":epoch|0,"count":n,
//    "intervals":[{"usid":..,"start":..,"stop":..,"wh":..}, ... newest first]}
String toJson();

// Live summary accessors for wb_buildStatusJson() (best-effort cross-task).
bool     chargingNow();        // a burst is currently open
uint32_t openSinceEpoch();     // start epoch of the open burst, else 0
uint8_t  count();              // stored interval count
uint32_t lastBurstWh();        // Wh of the most recently closed burst

// Wipe stored intervals (manual reset).
void clear();

}  // namespace wb_charge_log
