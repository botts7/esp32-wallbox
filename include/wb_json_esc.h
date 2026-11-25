#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wb_json_esc {

// Append a device- or radio-reported byte sequence as safe JSON string
// content (without surrounding quotes).
//
// - Escapes `"` and `\`.
// - Drops ASCII control characters (< 0x20) and DEL (0x7F).
// - Preserves valid UTF-8 multi-byte sequences (e.g. non-ASCII SSIDs or BLE
//   names) while dropping stray or truncated invalid UTF-8 bytes so strict
//   UTF-8 JSON parsers never fail on garbage advertising data (#88).
template <typename Out>
inline void appendEscaped(const char* data, size_t len, Out& out) {
    if (!data || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = static_cast<uint8_t>(data[i]);
        if (b < 0x80) {
            if (b == '"' || b == '\\') {
                out += '\\';
                out += static_cast<char>(b);
            } else if (b >= 0x20 && b != 0x7F) {
                out += static_cast<char>(b);
            }
            continue;
        }
        // Valid 2-byte UTF-8: C2..DF 80..BF
        if (b >= 0xC2 && b <= 0xDF) {
            if (i + 1 < len) {
                uint8_t b1 = static_cast<uint8_t>(data[i + 1]);
                if ((b1 & 0xC0) == 0x80) {
                    out += static_cast<char>(b);
                    out += static_cast<char>(b1);
                    i += 1;
                }
            }
            continue;
        }
        // Valid 3-byte UTF-8: E0..EF, excluding overlong (E0 80..9F) and
        // UTF-16 surrogates (ED A0..BF).
        if (b >= 0xE0 && b <= 0xEF) {
            if (i + 2 < len) {
                uint8_t b1 = static_cast<uint8_t>(data[i + 1]);
                uint8_t b2 = static_cast<uint8_t>(data[i + 2]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 &&
                    !(b == 0xE0 && b1 < 0xA0) &&
                    !(b == 0xED && b1 >= 0xA0)) {
                    out += static_cast<char>(b);
                    out += static_cast<char>(b1);
                    out += static_cast<char>(b2);
                    i += 2;
                }
            }
            continue;
        }
        // Valid 4-byte UTF-8: F0..F4, excluding overlong (F0 80..8F) and
        // code points above U+10FFFF (F4 90..BF).
        if (b >= 0xF0 && b <= 0xF4) {
            if (i + 3 < len) {
                uint8_t b1 = static_cast<uint8_t>(data[i + 1]);
                uint8_t b2 = static_cast<uint8_t>(data[i + 2]);
                uint8_t b3 = static_cast<uint8_t>(data[i + 3]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 &&
                    (b3 & 0xC0) == 0x80 &&
                    !(b == 0xF0 && b1 < 0x90) &&
                    !(b == 0xF4 && b1 >= 0x90)) {
                    out += static_cast<char>(b);
                    out += static_cast<char>(b1);
                    out += static_cast<char>(b2);
                    out += static_cast<char>(b3);
                    i += 3;
                }
            }
            continue;
        }
        // Stray continuation byte (80..BF), overlong lead (C0..C1), or
        // invalid lead (F5..FF): drop.
    }
}

}  // namespace wb_json_esc
