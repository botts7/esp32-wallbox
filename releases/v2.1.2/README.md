# Wallbox Gateway v2.1.2 — Schedule DST fix

Patch on top of [v2.1.1](../v2.1.1/README.md). Bug fix only.

## What's fixed

- **Schedule times were ±1 hour off depending on DST status of the user's timezone.** Conversions between UTC (charger storage) and the user's local clock used a hardcoded `Jan 1, 2024` reference date, which is in DST for the northern hemisphere and out of DST for the southern hemisphere. Now uses today's date, so the offset is always correct year-round.
  - Affected: both `utcToLocal` (display) and `localToUtc` (save). Schedules typed in our app may have been saved 1 hour off if you set them during one DST phase and viewed them in the other.
  - After flashing, reload the schedules page on your phone to see the correct times. If you have schedules that were stored with the wrong UTC value, re-save them (Edit → Save) to fix.

## Files

| File | Size | SHA256 |
|------|------|--------|
| `wallbox-gateway-esp32s3-v2.1.2.bin` | ~1.1 MB | `8306481d19de966d330548b47d86290e62a25a975ba9b8c7280b51793573e261` |
| `bootloader.bin` | 15 KB | `1776e4dd896a69d0a5c2e79957b0e2a88aa4129b1381d6478683515a1f6af343` |
| `partitions.bin` | 3 KB | `2f90ce5a68d5d487160953f0df402819f8ac594671296c0a2875fa3e4e7ef18e` |

## Install

OTA from any v2.x: open `http://wallbox-gw.local/ota` → upload the `.bin`.
For fresh installs see the [v2.1.0 README](../v2.1.0/README.md#installation).
