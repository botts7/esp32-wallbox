---
name: Charger compatibility report
about: Tested a charger model that isn't in the list
labels: compatibility
---

Great — compatibility reports help everyone!

**Charger model** (e.g. Pulsar Plus, Commander 2, Copper SB, Quasar 2):

**Charger firmware version:**

**BLE radio** (check GATT 0x180A service, or the Info page "BLE Module FW"):
- Manufacturer:
- Model:
- Firmware:

**GATT UUIDs used** (from Info → Charger Details, or dump of GATT services):
- Service UUID:
- Characteristic UUID:

**Compatibility checklist**
- [ ] BLE connects successfully
- [ ] r_dat (status) works
- [ ] r_sta (realtime) works
- [ ] w_cha (start/stop) works
- [ ] w_mxI (set current) works
- [ ] r_schs (schedules) works
- [ ] r_dca (energy meter) works
- [ ] HA entities populate correctly
- [ ] Any quirks or differences from Pulsar MAX behavior?

**Notes:**
Any observations — status codes that differ, extra fields, commands that don't work, etc.
