# v2.6.1

Quick fix for [#11](https://github.com/botts7/esp32-wallbox/issues/11)
from @mvanlijden: a Pulsar MAX on firmware 6.11.26 looped forever on
`Service 2456e1b9-... not found` because at that firmware Wallbox
migrated the MAX over to the dual-char Nordic-UART-style BLE
protocol that previously only shipped on Plus / Copper / Quasar.
Picking "Pulsar MAX (single-char)" in the gateway's `/config`
dropdown was correct for the hardware but no longer correct for the
on-charger protocol.

## What's fixed

When the BLE GATT scan completes and the configured-model service
UUID isn't present, the gateway now probes for the **other**
family's service UUID. If that's present, the gateway adopts the
correct protocol inline:

- updates `_svcUUID` / `_chrUUID` / `_txChrUUID` in memory
- updates `cfg.chargerModel` + `cfg.bleService` / `cfg.bleChar` /
  `cfg.bleTxChar` and `configMgr.save()`s to NVS so subsequent
  boots come up clean without manual intervention
- logs a giant block message so the user knows the switch
  happened and what model is now in effect
- falls through to the existing characteristic lookup
  (no disconnect/reconnect cycle)

Both directions are handled — MAX→Plus auto-switch for FW 6.11.26+,
Plus→MAX if someone mis-set the dropdown the other way.

## Verification

Tested on the maintainer's MAX by deliberately misconfiguring
`chargerModel` to "plus" via `/config`, rebooting, and watching the
log for the auto-switch. Confirmed:

- `[BLE] Auto-switching protocol family: configured Plus
  (dual-char), but charger speaks MAX (single-char) — adopting it
  and saving config.`
- `[Config] Saved to NVS`
- The existing MAX on FW 6.11.16 continued the connect flow inline:
  `[BLE] Notifications enabled` / `[BLE] Device: u-blox / NINA-B22`
  without further intervention

## Flashing

OTA from 2.4.1+:
```bash
curl -u admin:<otapass> -F firmware=@firmware.bin http://<gw>/api/ota
```

USB:
```bash
python -m platformio run --target upload
```
