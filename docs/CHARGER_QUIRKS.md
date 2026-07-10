# Charger-Model Quirks & Differences

A consolidated reference of every charger-model-specific behaviour across the
three repos (firmware `esp32-wallbox`, integration `hass-wallbox-gateway`,
add-on `wallbox-gateway-ha-addon`). Use this when touching anything that reads
r_dat/r_sta/r_lse/r_dca, selects a command/`par`, or maps a status enum — a
change that is correct for the MAX (the reference model) can silently break
Plus / Copper / Quasar / Zentri.

Testing status (see `COMPATIBILITY.md`): **MAX = reference, fully tested**;
Plus = partially verified (peter-mcc #4); **Copper / Quasar / Quasar2 =
untested/scaffold**; **Zentri / original Pulsar = single-source (gambys #12)**.

## 1. Model taxonomy & detection

| Model | Config key | Transport | Detected |
|---|---|---|---|
| Pulsar MAX | `max` (default) | single-char u-blox (`2456e1b9…d701/d703`) | `chargerModel`; svc UUID |
| Pulsar Plus | `plus` | dual-char BGX13P (`331a36f5…`, notify `a73e9a10…`) | `chargerModel`; svc UUID |
| Copper SB/Business | `copper` | dual-char (Plus family) | `chargerModel` |
| Quasar / Quasar2 | `quasar`/`quasar2` | dual-char (Plus family) | `chargerModel` |
| Zentri / original Pulsar | usually `custom`/`max` | TruConnect (`175f8f23`, MODE `20b9794f`) | **runtime** MODE char → `_isZentri` (`wb_ble.cpp:568-571`) |

- Family predicate `isPlus()` / `isPlusFamily()` = `plus|copper|quasar|quasar2`
  (`wb_ble.h:118-120`, `wb_config.h:96-99`). **Neither includes Zentri** —
  `_isZentri` is a runtime-only flag that does NOT feed these predicates. This
  is the root of several Zentri bugs (keepalive, stop-par).
- MAX↔Plus auto-adopt: configured MAX but charger speaks Plus dual-char (MAX
  firmware ≥6.11.26 migrated) → adopt + persist Plus UUIDs, no disconnect
  (`wb_ble.cpp:484-537`). Only rescues MAX↔Plus; a `custom` Zentri with a wrong
  UUID gets config-mismatch + 60s backoff.

## 2. r_dat / field availability per model

| Field | MAX | Plus | Zentri | Copper | Quasar |
|---|---|---|---|---|---|
| `st` | 0..18 | 0..18 | **small enum 0..4** (4=ramp) | ? (scaffold) | 0..18 (+11 discharge) |
| `cp` (kW) | native | native | **absent → synthesized** from L1/L2/L3 (`wb_zentri_normalize.cpp:54-61`) | absent (TODO) | native |
| `en` (session kWh) | centi-kWh | centi-kWh | **absent → 0** | ? | yes |
| `gen` | **override flag** (not energy) | **override flag** | absent | ? | override flag |
| `grid` | yes | yes | absent | ? | yes |
| `den` (V2H) | yes | yes | absent | absent | yes (core) |
| `L1/L2/L3` (deci-A) | yes | yes | **only power source** | **"only Current L1"** | yes |
| `cur` (max A) | yes | yes (fallback) | yes | ? | yes |
| `r_sta.cm/ic` | may be absent | **often absent → r_dat.cur fallback** | absent | absent | present |
| `r_lse.green/grid` | yes (authoritative) | yes | likely absent | ? | yes |

## 3. Scale factors (get these wrong and a model reads 10×/100× off)

| Value | Raw field | Divisor | Notes |
|---|---|---|---|
| Session energy | `r_dat.en` | **÷100** (centi-kWh) | `Δen×10 = Wh` |
| Grid / green energy | `r_dat.grid` / `r_dat.gen` | ÷100 | but `gen` is a FLAG on MAX/Plus, not energy |
| V2H discharge | `r_dat.den` | **÷1000 (fw)** vs **÷100 (integration)** | ⚠ 10× MISMATCH — bug |
| Phase currents | `r_dat.L1/L2/L3`, `r_dca.c1/c2/c3` | ÷10 (deci-A) | |
| Meter lifetime | `r_dca.e` | ÷1000 (Wh) | |
| **Session-history** energy | `r_ses`/`r_log.en` | **÷1000 (Wh)** | same key `en`, **10× different** from r_dat.en |
| Charge-log burst | firmware `wh` | ÷1000 (Wh) | matches r_ses |
| LSE session green/grid | `r_lse.green_energy`/`grid_energy` | ÷1 (already kWh) | **authoritative solar source** |

## 4. Status enum

- MAX/Plus: 0..18 (`wb_mqtt.cpp:949-956`, `const.py:279-299`).
- Zentri: 0=ready,1=charging,2=connected,3=waiting,4=ramp
  (`wb_zentri_normalize.cpp:42`, `const.py:305-311`).
- Integration + add-on select the right table per model; **firmware MQTT status
  sensor and `car_connected` do NOT** — they apply the MAX table to Zentri/Copper.

## 5. Control commands per model

| Feature | MAX | Plus / Copper / Quasar | Zentri |
|---|---|---|---|
| Start | `w_cha` par=1 | par=1 | par=1 |
| **Stop** | par=**2** (hard) | par=**0** (pause) — Plus ignores par=2 (#4) | par by *config* family ⚠ |
| Set current | `w_mxI` | `w_mxI` | **can't over BLE** (`charger_control.py:144`) |
| Schedules read | `r_schs` array | `r_schs` array | per-sid `r_sch` loop |
| Schedule delete | `clr_sch` (no fallback) | `clr_sch` | `clr_sch` |
| Eco-Smart/Solar | `s_ecos` if meter | if meter | typically none |
| Resume (`s_cmode {mode:0}`) | Stop-prefix only if charging (else err 114) | same | same |
| Lock / reboot(charger) | `w_lck` / `rebot` | same | same |
| Halo | `s_halocfg` (MQTT = fixed "2" placeholder #158) | same | likely absent |
| V2H | n/a | n/a | n/a; Quasar `s_pwi`/`r_dis` defined, **no control wiring** |

## 6. Latent bugs (ranked; see change-set)

**Correctness / safety**
1. **Charge-log green uses `r_dat.gen` as energy** → bogus per-burst solar Wh on
   MAX/Plus (gen is the override flag). Source green from `r_lse`
   (`wb_charge_log.cpp:194,165-167`).
2. **Charge-log capture gates only on `cp > 0.10`** → misses low-cp Eco-Smart
   solar; add `Δen` detection (`wb_charge_log.cpp:189,198`).
3. **`den` 10× scale mismatch** — fw ÷1000 vs integration ÷100 (`wb_mqtt.cpp:943`
   vs `sensor.py:342`).
4. **MQTT `green_energy = r_dat.gen/100`** publishes the override flag as a kWh
   sensor (`wb_mqtt.cpp:937-938`); integration already removed its copy.
5. **MQTT `resume_schedule` sends hard-Stop unconditionally** → can fault a
   *paused* charger (error 114). Gate on `isCharging()` like the web path
   (`wb_mqtt.cpp:612-619` vs `wb_web_async.cpp:962-966`).
6. **Web `action=current` unclamped** (`wb_web.cpp:1099`, `wb_web_async.cpp:971`)
   — only MQTT/integration clamp 6–32.

**Stability**
7. **BLE notify/response cross-core race** — the NimBLE callback (core 0) mutates
   the parser/response with no lock vs `_sendCommandDirect` (core 1) reset →
   panic on marginal links (`wb_ble.cpp:1086-1095` vs `1279-1356`).
8. **Integration fires 9 concurrent `action=bapi` passthroughs every 10s** for
   already-cached data (`coordinator.py:108-130`) → token-bucket 429 storm +
   sustained BLE pressure amplifying #7.

**Per-model (Zentri/Copper — untested models)**
9. **Zentri keepalive uses `ping`** (chosen by `isPlus()`, false for Zentri) →
   reconnect loop if original Pulsar lacks `ping` (`wb_ble.cpp:196`).
10. **Zentri stop-par chosen by config family, not runtime `_isZentri`** → a
    default-`max` Zentri sends par=2 (`wb_web_async.cpp:953`).
11. **Firmware MQTT status + `car_connected` apply MAX enum to Zentri/Copper**
    (`wb_mqtt.cpp:755-763,949-956`).
12. **Zentri set-current entity created + web/MQTT accept it** though BLE can't
    set current (`charger_control.py:144`).
13. **Copper normalize is an empty scaffold** (`wb_copper_normalize.cpp:23-55`) →
    raw Copper fields read with MAX assumptions.
14. **eco/solar/halo control entities created for every model, no `available()`
    gating** → no-op/error on models without the feature.
15. **No `clr_sch` delete fallback** for a model that rejects it
    (`schedule.py:243-247`).
16. **MQTT Halo state is a fixed placeholder "2"** on all models (#158).

**Auth / connection**
17. **`_authenticate()` never fails** — wrong/missing PIN still reports CONNECTED
    then writes are silently rejected (`wb_ble.cpp:1035-1055`).
18. **MAX↔Plus auto-adopt persists NVS from the BLE core-1 task**
    (`configMgr.save()` at `wb_ble.cpp:528`) — confirm concurrency safety.
