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
// (BAPI r_dat field `cp`, kW) on each realtime sample and recording every
// charging *burst* (a contiguous cp>0 period) as an interval. A session can
// have many intervals (pause/resume, dynamic load, solar throttle); a night
// with no charging has none. It is pure observation of cp, so it captures
// manual / unscheduled charging exactly as well as scheduled.
//
// Intervals are mirrored to NVS (like wb_diag) so reboots don't wipe history.
// Write churn is bounded — one NVS write per burst close, nothing per sample.
// Forward-only: it cannot reconstruct sessions that finished before this ran.

namespace wb_charge_log {

// Ring size. Each interval is small; ~24 covers a few days of typical use.
static const uint8_t MAX_INTERVALS = 24;

// Charging is "on" when cp exceeds this (kW) — filters meter noise / standby.
static constexpr float CP_ON_KW = 0.10f;

// Initialise (loads the persisted ring count). Call once after NVS + NTP.
void begin();

// Feed every new realtime r_dat JSON sample (the full {"r":{...}} response).
// Detects burst rising/falling edges and records closed bursts. Cheap; safe
// to call on every realtime poll. Must be called from a single task (the main
// task's realtime drain) — it owns the open-burst state.
void onRealtime(const String& rdatJson);

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
