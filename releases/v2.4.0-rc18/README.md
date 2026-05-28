# v2.4.0-rc18

Two small UI fixes on top of rc17 — no protocol / BLE / MQTT changes.

## Nav highlight on /logs

The Log page passed the wrong active-path string to the footer, so the
Info button lit up blue while the user was actually viewing /logs.
Fixed.

## Oversized SVG nav icons on stale CSS

The nav SVG sizing rule (`.nav-item svg { width:22px; height:22px }`)
was only in `/style.css`, not in the inline bootstrap style block. If
the browser cached a pre-rc12 `/style.css` for a moment after an OTA,
the SVGs would render at their native 300×150 default while waiting
for fresh CSS — giving a moment of giant black nav icons before things
settled.

Fixed by adding explicit `width='22' height='22'` attributes on each
SVG so the size is correct regardless of CSS load state. The CSS rule
still applies for any future restyling.

## SHA256

See `SHA256SUMS.txt`.

## Installation

Existing rc7+ gateways OTA via `/ota` (auth required since rc11). Fresh
USB installs via `install.json`.
