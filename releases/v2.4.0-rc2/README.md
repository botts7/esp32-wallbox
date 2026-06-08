# v2.4.0-rc2 — Pulsar Plus pre-release (CRLF fix)

⚠️ **Release candidate — for compatibility testers only.** Pulsar MAX
users on a stable v2.3.0 setup should not upgrade unless they want to
help test.

Builds on [v2.4.0-rc1](../v2.4.0-rc1/README.md) with one fix from real-world feedback.

## Fixed in rc2

- **Telnet log emits CRLF, not bare LF.** Raw TCP clients on Windows
  (built-in `telnet.exe`, PowerShell, `nc` without translation) now
  render lines correctly without configuring CR-injection in the client.
  Reported by [@peter-mcc](https://github.com/peter-mcc) in issue
  [#2](https://github.com/botts7/esp32-wallbox/issues/2).
- README + `COMPATIBILITY.md` updated with platform-specific telnet
  notes ([PuTTY](https://www.putty.org/) remains the easiest path on
  Windows; built-in telnet works too once enabled).

## Everything else from rc1

Same as [rc1](../v2.4.0-rc1/README.md): charger-model dropdown, dual
RX/TX characteristic support, Pulsar Plus preset UUIDs. Pulsar MAX
behaviour unchanged (smoke-tested).

## How to test (Pulsar Plus owners)

Same as rc1. OTA from any v2.x, then in `http://wallbox-gw.local/config`
→ Advanced → **Charger model** = `Pulsar Plus` → Save & reboot.

Telnet on Windows: either install [PuTTY](https://www.putty.org/) (Raw
mode, port 23) or enable the built-in client (`dism /online /Enable-Feature
/FeatureName:TelnetClient`).

## SHA256

```
b897e443fc7aaafd2d7b58e373d24121c5e32457bea76763d9f8cab6070e0f1e  wallbox-gateway-esp32s3-v2.4.0-rc2.bin
1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343  bootloader.bin
2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e  partitions.bin
```
