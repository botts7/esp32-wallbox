"""PlatformIO pre-script: derive WB_VERSION from git at build time.

- On a release-tag commit:   v2.4.0-rc4
- One commit after a tag:    v2.4.0-rc4-1-gabc1234
- With uncommitted changes:  v2.4.0-rc4-1-gabc1234-dirty
- Without git available:     unknown

Single source of truth — no hardcoded version strings to keep in sync.
"""
import subprocess

Import("env")  # type: ignore  (PlatformIO injects this)


def _git_version() -> str:
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty", "--match", "v*"],
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip() or "unknown"
    except Exception:
        return "unknown"


version = _git_version()
print(f"[version.py] WB_VERSION = {version}")

env.Append(CPPDEFINES=[("WB_VERSION", env.StringifyMacro(version))])  # type: ignore
