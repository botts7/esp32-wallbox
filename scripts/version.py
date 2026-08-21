"""PlatformIO pre-script: derive WB_VERSION from git at build time.

- On a release-tag commit:   v2.4.0-rc4
- One commit after a tag:    v2.4.0-rc4-1-gabc1234
- With uncommitted changes:  v2.4.0-rc4-1-gabc1234-dirty
- Without git available:     unknown

Single source of truth — no hardcoded version strings to keep in sync.
"""
import subprocess

Import("env")  # type: ignore  (PlatformIO injects this)


def _is_generated(path: str) -> bool:
    # The pre-gzipped web-page headers are regenerated on every build, so they
    # dirty the tree even on an otherwise-clean release checkout. Ignore them
    # when deciding "-dirty" — otherwise every release binary reports -dirty
    # (which confused users comparing versions, e.g. #13).
    return path.startswith("include/_gen_") and path.endswith("_body_gz.h")


def _is_dirty() -> bool:
    """Tree has real (non-generated) tracked changes."""
    try:
        out = subprocess.check_output(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            stderr=subprocess.DEVNULL,
        ).decode()
    except Exception:
        return False
    for line in out.splitlines():
        path = line[3:].strip()
        if path and not _is_generated(path):
            return True
    return False


def _git_version() -> str:
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--match", "v*"],
            stderr=subprocess.DEVNULL,
        )
        version = out.decode().strip() or "unknown"
    except Exception:
        return "unknown"
    if version != "unknown" and _is_dirty():
        version += "-dirty"
    return version


version = _git_version()
print(f"[version.py] WB_VERSION = {version}")

env.Append(CPPDEFINES=[("WB_VERSION", env.StringifyMacro(version))])  # type: ignore

# Board/build target = the PlatformIO env name (e.g. "esp32s3"), which is also
# the suffix on the release binary asset (wallbox-gateway-<ver>-esp32s3.bin).
# Surfaced in /api/status.board so the HA integration's Update entity can fetch
# the matching asset for OTA. Single source of truth — no separate board string.
# The `ota` env is only a convenience for `pio run -e ota -t upload` (espota);
# it EXTENDS esp32s3, so report the real target it builds — otherwise a gateway
# flashed that way reports board="ota" and the Update entity finds no asset.
_ENV_BOARD_ALIAS = {"ota": "esp32s3"}
board = _ENV_BOARD_ALIAS.get(env["PIOENV"], env["PIOENV"])  # type: ignore
print(f"[version.py] WB_BOARD = {board}")
env.Append(CPPDEFINES=[("WB_BOARD", env.StringifyMacro(board))])  # type: ignore
