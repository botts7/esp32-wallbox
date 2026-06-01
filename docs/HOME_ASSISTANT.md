# Home Assistant Integration Guide

This gateway publishes charger state and accepts commands via MQTT with auto-discovery.
All entities appear automatically under a single device: **Wallbox Pulsar MAX**.

## Recommended Setup

### 1. Local control (this gateway)
- BLE → Home Assistant over MQTT
- 10-second updates for status, power, current
- 30-second updates for schedules/settings
- All control (start/stop, current, lock, schedules) works offline

### 2. Cloud data (optional)
For session cost, billing history, and monthly totals — add the
[official Wallbox integration](https://www.home-assistant.io/integrations/wallbox/) in HA.
It polls the Wallbox cloud API and complements this gateway's local data.

We intentionally don't do cloud polling on the ESP32 — it would fight BLE for radio time
and reduce responsiveness.

## Entities Created

### Live status (10s updates)
- `sensor.wallbox_charging_power` — kW
- `sensor.wallbox_current_l1` / `_l2` / `_l3` — Amps per phase
- `sensor.wallbox_energy_session` — kWh this session
- `sensor.wallbox_total_energy` / `_green_energy` — kWh total / solar
- `sensor.wallbox_status` — human-readable state (Ready, Charging, Plugged In, etc.)
- `sensor.wallbox_mains_voltage` — V from meter
- `sensor.wallbox_grid_power` — W from meter
- `binary_sensor.wallbox_car_connected` — plug state
- `sensor.wallbox_ocpp_status` — Not Configured / Connected / Charging

### Controls
- `switch.wallbox_charging` — start/stop
- `switch.wallbox_lock` — lock/unlock socket
- `switch.wallbox_autolock` — auto-lock enabled
- `switch.wallbox_power_sharing` — dynamic power sharing
- `switch.wallbox_phase_switch`
- `number.wallbox_max_charging_current` — 6-32A slider
- `number.wallbox_autolock_timeout` — 1-60 min
- `number.wallbox_eco_smart_solar_percent` — 0-100%
- `select.wallbox_eco_smart_mode` — Off / Solar + Grid / Full Green
- `select.wallbox_halo_led` — Off / Low / Medium / High
- `select.wallbox_timezone`
- `button.wallbox_reboot_charger`

### Diagnostics
- `sensor.wallbox_ble_signal` — dBm
- `sensor.wallbox_charger_name` / `_manufacturer` / `_model`
- `sensor.wallbox_ble_firmware`

## Dashboard Examples

### Drop-in full dashboard

A ready-to-paste Lovelace YAML covering hero status, controls, live data, 24h
graph, and Energy dashboard integration:

→ **[`docs/LOVELACE_CARD.yaml`](LOVELACE_CARD.yaml)** — copy the contents into
a new Lovelace dashboard (Edit Dashboard → ⋮ → Raw configuration editor).
No custom cards required; works on any HA ≥ 2024.1.

Adjust the gauge `max` to match your charger (Pulsar MAX 1-phase 32 A ≈ 7.4 kW;
3-phase ≈ 22 kW) and the entity names if you customised the HA device name.

### Simple charging card

```yaml
type: entities
title: Wallbox
entities:
  - entity: sensor.wallbox_status
  - entity: sensor.wallbox_charging_power
  - entity: sensor.wallbox_energy_session
  - entity: switch.wallbox_charging
  - entity: number.wallbox_max_charging_current
  - entity: switch.wallbox_lock
```

### Live power graph

```yaml
type: history-graph
title: Charging Power
hours_to_show: 24
entities:
  - sensor.wallbox_charging_power
  - sensor.wallbox_grid_power
```

### Quick status card

```yaml
type: glance
title: Wallbox
entities:
  - entity: sensor.wallbox_status
  - entity: sensor.wallbox_charging_power
  - entity: binary_sensor.wallbox_car_connected
  - entity: sensor.wallbox_ble_signal
```

## Automation Recipes

### Start charging when solar surplus exceeds 2kW

```yaml
alias: Start charging on solar surplus
trigger:
  - platform: numeric_state
    entity_id: sensor.solar_export
    above: 2000
    for: '00:05:00'
condition:
  - condition: state
    entity_id: binary_sensor.wallbox_car_connected
    state: 'on'
  - condition: state
    entity_id: switch.wallbox_charging
    state: 'off'
action:
  - service: switch.turn_on
    entity_id: switch.wallbox_charging
  - service: number.set_value
    entity_id: number.wallbox_max_charging_current
    data:
      value: 16
```

### Notify when charge complete

```yaml
alias: Charge complete notification
trigger:
  - platform: state
    entity_id: sensor.wallbox_status
    to: 'Charge Complete'
action:
  - service: notify.mobile_app
    data:
      title: EV Charging Complete
      message: 'Session: {{ states("sensor.wallbox_energy_session") }} kWh'
```

### Off-peak charging schedule

```yaml
alias: Start off-peak charge
trigger:
  - platform: time
    at: '00:00:00'
action:
  - service: switch.turn_on
    entity_id: switch.wallbox_charging
  - service: number.set_value
    entity_id: number.wallbox_max_charging_current
    data:
      value: 32

alias: Stop charge at 6am
trigger:
  - platform: time
    at: '06:00:00'
action:
  - service: switch.turn_off
    entity_id: switch.wallbox_charging
```

### Enable Eco Smart when home battery is full

```yaml
alias: EV eco smart when battery full
trigger:
  - platform: numeric_state
    entity_id: sensor.home_battery_soc
    above: 95
action:
  - service: select.select_option
    entity_id: select.wallbox_eco_smart_mode
    data:
      option: 'Solar + Grid'
```

## Cost Tracking (Time-of-Use Rates)

**Why HA, not the gateway?** Tariff schedules in the Wallbox app are a **cloud-only feature** —
the charger firmware doesn't expose tariff BAPI commands (it only stores a single default
energy price). Home Assistant does this much better with `utility_meter` + tariffs, and it
works completely offline.

### Simple: Peak / Off-Peak rates

```yaml
# configuration.yaml
utility_meter:
  wallbox_session_meter:
    source: sensor.wallbox_pulsar_max_session_energy
    cycle: daily
    tariffs: [peak, offpeak]

# Switch tariff by time of day
automation:
  - alias: Wallbox offpeak rate (11pm)
    trigger:
      - platform: time
        at: '23:00:00'
    action:
      - service: select.select_option
        target: {entity_id: select.wallbox_session_meter}
        data: {option: offpeak}

  - alias: Wallbox peak rate (7am)
    trigger:
      - platform: time
        at: '07:00:00'
    action:
      - service: select.select_option
        target: {entity_id: select.wallbox_session_meter}
        data: {option: peak}

# Cost = (peak kWh × peak rate) + (offpeak kWh × offpeak rate)
sensor:
  - platform: template
    sensors:
      wallbox_session_cost:
        friendly_name: "Session Cost"
        unit_of_measurement: "$"
        value_template: >
          {% set peak = states('sensor.wallbox_session_meter_peak') | float(0) %}
          {% set off = states('sensor.wallbox_session_meter_offpeak') | float(0) %}
          {{ ((peak * 0.45) + (off * 0.22)) | round(2) }}
```

### Advanced: 4-tier with weekday/weekend differences

```yaml
utility_meter:
  wallbox_meter:
    source: sensor.wallbox_pulsar_max_session_energy
    cycle: daily
    tariffs: [peak, shoulder, offpeak, weekend]

automation:
  - alias: Wallbox tariff controller
    trigger:
      - platform: time_pattern
        hours: '*'
    action:
      - service: select.select_option
        target: {entity_id: select.wallbox_meter}
        data:
          option: >
            {% set h = now().hour %}
            {% set wd = now().weekday() %}
            {% if wd >= 5 %}weekend
            {% elif 17 <= h < 21 %}peak
            {% elif 7 <= h < 17 or 21 <= h < 23 %}shoulder
            {% else %}offpeak
            {% endif %}

sensor:
  - platform: template
    sensors:
      wallbox_session_cost:
        unit_of_measurement: "$"
        value_template: >
          {% set peak = states('sensor.wallbox_meter_peak') | float(0) %}
          {% set shoulder = states('sensor.wallbox_meter_shoulder') | float(0) %}
          {% set off = states('sensor.wallbox_meter_offpeak') | float(0) %}
          {% set wk = states('sensor.wallbox_meter_weekend') | float(0) %}
          {{ (peak*0.52 + shoulder*0.35 + off*0.18 + wk*0.28) | round(2) }}
```

### Monthly total with auto-reset

```yaml
utility_meter:
  wallbox_monthly:
    source: sensor.wallbox_pulsar_max_lifetime_energy
    cycle: monthly
```

Then `sensor.wallbox_monthly` resets on the 1st of each month automatically.

### HA Energy Dashboard

1. Settings → Dashboards → Energy
2. Add Individual Device: `sensor.wallbox_pulsar_max_lifetime_energy` (source: "Grid")
3. Set cost: either flat rate or link to a cost sensor (like above)
4. Automatic daily/monthly reports with cost breakdowns

### Alternative: official HA Wallbox integration

If you want session cost/billing data from Wallbox's own cloud (tariffs set in the app),
add the [official Wallbox integration](https://www.home-assistant.io/integrations/wallbox/)
in HA — it pulls session history and costs from the Wallbox cloud API. Our gateway
complements it by providing fast local control and live data without cloud dependency.

## Statistics Setup

For long-term energy tracking, configure these as Statistics in HA (Settings > Devices > Statistics):

- `sensor.wallbox_total_energy` — meter type: total_increasing
- `sensor.wallbox_green_energy` — meter type: total_increasing
- `sensor.wallbox_energy_session` — state_class: total_increasing

Then add them to your Energy Dashboard under "Individual Devices" for historical views.

## Troubleshooting

### Entities show "Unavailable"
- Check gateway web UI at `http://wallbox-gw.local/` — is BLE connected?
- Gateway marks entities offline after 60s of BLE disconnect
- BLE drops during OTA updates — normal, restores after reboot

### Commands not responding
- Check `Charger Status` entity — if "Error" or "Locked", commands may be blocked
- Touch the Wallbox keypad to wake from deep sleep

### Can't find device
- Ensure MQTT is configured in gateway config (`http://wallbox-gw.local/config`)
- HA MQTT integration must be set up with auto-discovery enabled
