#pragma once

// WB_VERSION is injected at build time by scripts/version.py via PlatformIO,
// derived from `git describe --tags --dirty`. This header just provides a
// fallback so the code still compiles in an IDE that hasn't run the pre-script
// (or in CI without git history). See platformio.ini → extra_scripts.
#ifndef WB_VERSION
#define WB_VERSION "dev"
#endif
