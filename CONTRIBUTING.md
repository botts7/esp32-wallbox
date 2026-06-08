# Contributing

Thanks for your interest! Contributions are welcome.

## Before you start

**Open an issue first** for anything non-trivial. A quick discussion saves everyone time —
we might already be working on it, or there might be reasons we went a different direction.

## Development setup

1. Install [PlatformIO](https://platformio.org/install) (VS Code extension recommended)
2. Clone this repo
3. Open the `esp32-wallbox` folder in VS Code
4. Connect an ESP32-S3 via USB
5. `pio run -e esp32s3 -t upload` to flash
6. `pio device monitor` to see serial output

## Types of contributions

### Charger compatibility reports
Probably the most valuable — test with your charger and open an issue using the
`charger_compatibility` template. Even reporting "works exactly like Pulsar MAX" is useful.

### Bug fixes
Pull requests for bug fixes are welcome. Include:
- What the bug is
- How to reproduce it
- What your fix does
- Serial log before/after if relevant

### New features
Discuss in an issue first. The project aims to stay **focused**:
- ✅ Wallbox-specific features
- ✅ Home Assistant integration improvements
- ✅ BLE/WiFi stability
- ✅ Documentation
- ❌ General-purpose ESP32 frameworks
- ❌ Cloud polling (we explicitly don't do this — use the official HA Wallbox integration)

### Documentation
Always appreciated. The HA integration doc especially benefits from real automation examples.

## Code style

- C++ follows Arduino conventions (camelCase methods, PascalCase classes)
- Short comments explaining **why**, not what
- JSON serialization uses `ArduinoJson` v7
- Web UI uses inline styles for one-offs, CSS classes for reused patterns
- BAPI method constants in `include/bapi.h`, never hardcode strings

## Testing

The project has an integration test suite — run against a live gateway:

```python
# See previous test runs in conversation history for reference
# Tests expect gateway at http://wallbox-gw.local/ or configured IP
python tests/integration_test.py
```

Before submitting a PR:
- [ ] `pio run -e esp32s3` builds clean
- [ ] Tested on actual hardware
- [ ] CHANGELOG.md updated under `[Unreleased]` section
- [ ] README updated if adding user-visible features

## Commit messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
feat: short description of what was added
fix: short description of what was fixed
docs: documentation changes
refactor: code changes with no behavior change
chore: build/release tooling
```

Keep commits focused — one logical change per commit.

## Legal

By contributing, you agree your contributions are licensed under the MIT License
(same as the project).

Don't submit code or assets you don't own or have rights to. Specifically:
- No code from sources that don't permit redistribution
- No proprietary protocol documentation
- No BAPI commands copied from non-public sources

The BAPI method names in this project are public-API string constants observed on the wire
during normal Wallbox device communication. Contributions that arrive through standard
interoperability research (EU Directive 2009/24/EC Art. 6) are welcome.
