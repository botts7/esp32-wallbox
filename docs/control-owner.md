# Charge Control Owner — arbitration design

**Status:** agreed 2026-06-20, build in progress. Not pushed (gated with v3.2).

## Problem

Up to three things can send start/stop to the charger: the Wallbox's own
on-device schedule, the HA **Integration** Charge Assistant (smart-charge /
solar / target), and a future **Add-on** Charge Assistant. Two autonomous
controllers acting at once fight (one stops, the other restarts). We need a
deterministic way to decide who's in charge — and to make conflicts visible.

## Principle

It should **just work**, but the user owns the variables in play. One setting
on the gateway picks the source of truth; everything else (pausing a
conflicting native schedule, making the non-chosen controllers inert) happens
automatically and is **surfaced** in every GUI.

## The one knob: `control_owner` (gateway config)

Stored in `WBConfig` (NVS), set on the gateway `/config` page, exposed on
`/api/status`. Values:

- `wallbox_schedule` — the charger's own schedule decides. **Default.**
- `integration` — the HA Integration's Charge Assistant decides.
- `addon` — the Add-on's Charge Assistant decides (future).
- `none` — manual / charge-on-plug-in only.

## Behaviour (owner drives everything)

| Owner | Native Wallbox schedule | Integration / Add-on controllers | Manual |
|---|---|---|---|
| `wallbox_schedule` | active (auto-resumed/enabled if disabled by us) | stand down | always works |
| `integration` | overlapping entries auto-disabled | integration acts; add-on stands down | always works |
| `addon` | overlapping entries auto-disabled | add-on acts; integration stands down | always works |
| `none` | left as the user set it | all stand down | manual / plug-in |

A controller only acts autonomously when `control_owner` equals its own id.
The non-owner shows a gentle notice ("Not the active charge controller — the
gateway is set to X"), never silently does nothing.

## Pause mechanism = disable the overlapping schedule entry

To stop the native schedule fighting an external owner we **disable just the
overlapping native schedule entries** via `s_sch` (`enabled:0`) — the same
write the dashboard's pause toggle already uses (proven). **No separate
`s_cmode` pause call needed** (that unknown is removed). Rules:

- Disable only the native entries that **overlap** when the external owner
  actually wants to charge differently — minimal interference.
- **Persist** each entry's prior enabled-state; **reconcile on every
  reconnect** so a crash can never strand a schedule off; restore on owner
  change.
- The owner setting is the source of truth; schedule enabled-state is derived
  from it.

`stop` commands are reserved for **manual override** (see below), not for
fighting the native schedule.

## Manual override

Every controller-issued start/stop is **owner-tagged** (`&owner=<id>`); the
gateway records `last_command_by` + timestamp. Manual commands carry no tag.
A controller backs off for a cooldown when it sees a recent command that
wasn't its own (manual or another controller). You can always grab the wheel.

## Advisory, not enforced

The gateway never **rejects** a command — it records owner/last-commander and
exposes them. Controllers voluntarily check and yield. This keeps **manual
control always working** (the gateway can't reliably tell autonomous from
manual except by the tag).

## Surface conflicts in every GUI

- **Gateway dashboard + Add-on:** show active control owner; banner when a
  native schedule is auto-disabled by external control ("Schedule #6 paused —
  HA Integration controls charging"); the schedule timeline already stripes
  schedule-vs-schedule overlaps — add an explicit list ("⚠ #6 and #8 overlap
  Sat 00:50–06:00").
- **Integration:** a `control_owner` sensor + an HA **Repair** when an acting
  mode is configured but the gateway owner isn't `integration`, or when two
  controllers are set up.

## Build order

1. **Firmware**: `control_owner` config field + `/config` dropdown + expose on
   `/api/status`; `last_command_by` tracking for owner-tagged `/api/command`.
2. **Integration**: read `control_owner`; only act when `== integration`;
   auto-disable/restore overlapping native schedules on ownership change;
   respect manual override; show the "not active controller" notice; add the
   sensor + Repair.
3. **Add-on**: same rules when its Charge Assistant is built; dashboard
   surfacing now.

## Wire surface (firmware)

- `/api/status` gains `control_owner` (string) + `last_command_by` (string)
  + `last_command_age_s` (int).
- `/api/command` accepts optional `&owner=<id>` on start/stop/current, records
  it as `last_command_by`. Advisory only — never rejects.
