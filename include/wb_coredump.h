#pragma once

#include <Arduino.h>

// Crash-dump readout over HTTP (#168).
//
// The panic handler writes a full ELF core to the `coredump` partition on
// every panic / watchdog reset. Getting it back out used to mean physically
// unplugging the gateway and reading it over serial with esp-coredump — which
// is impractical for a bug that fires once a day, and flatly impossible for a
// user reporting a crash on their own box halfway around the world.
//
// So we read it here instead:
//
//   GET  /api/coredump/summary  -> JSON: crashing TASK NAME, PC, exception
//                                  cause, and the raw backtrace PCs. This is
//                                  the answer to "which task died", and it
//                                  needs no ELF and no cable.
//   GET  /api/coredump          -> the raw ELF core, for full symbolisation
//                                  with esp-coredump against the matching
//                                  firmware.elf.
//   POST /api/coredump/clear    -> erase, so the next crash starts clean.
//
// The summary carries the crashing app's ELF SHA256 so you can prove the
// firmware.elf you're symbolising against is the one that actually crashed —
// symbolising a dump with a mismatched ELF yields a plausible, wrong
// backtrace, which is worse than no backtrace at all.
//
// All of this is inert on a device whose partition table predates the
// coredump partition (i.e. any install that has only ever been OTA'd):
// present() simply returns false. See partitions_ota.csv.

namespace wb_coredump {

// True when this device's partition table actually has a coredump partition.
// False on any install that has only ever been OTA'd (their table predates
// it), where no crash will ever be captured until a USB re-flash. Callers
// MUST distinguish this from present()==false: "no crash yet" and "can never
// record a crash" look identical otherwise, and the second one silently
// wastes days of waiting for evidence that was never coming.
bool partitionAvailable();

// True when flash holds a valid, checksum-verified core dump.
bool present();

// Size of the stored image in bytes (0 when absent/invalid).
size_t size();

// Crash summary as JSON. Always returns a valid JSON object — on a device
// with no dump (or no coredump partition at all) that's {"present":false}.
String summaryJson();

// Copy up to `len` bytes of the raw image starting at `offset`. Returns the
// number of bytes actually read (0 at/after EOF or on error). Used to stream
// the download in chunks rather than building a multi-hundred-KB String in RAM.
size_t readChunk(size_t offset, uint8_t* buf, size_t len);

// Erase the stored dump. Returns true on success.
bool erase();

}  // namespace wb_coredump
