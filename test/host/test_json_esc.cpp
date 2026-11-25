// Host unit test for wb_json_esc (issue #88: control chars in BLE scan names).
// Compile: g++ -std=c++11 -Iinclude test/host/test_json_esc.cpp -o test_json_esc
#include "wb_json_esc.h"
#include <cassert>
#include <cstdio>
#include <string>

static std::string esc(const std::string& in) {
    std::string out;
    wb_json_esc::appendEscaped(in.data(), in.size(), out);
    return out;
}

int main() {
    // 1. Exact #88 repro: trailing DLE (0x10) control character in BLE name.
    {
        std::string raw = std::string("EX12150250925054") + '\x10';
        assert(esc(raw) == "EX12150250925054");
    }

    // 2. All ASCII control chars 0x00..0x1F and DEL 0x7F are dropped.
    {
        std::string raw;
        raw.push_back('A');
        for (int c = 0; c < 0x20; c++) raw.push_back(static_cast<char>(c));
        raw.push_back('B');
        raw.push_back(static_cast<char>(0x7F));
        raw.push_back('C');
        assert(esc(raw) == "ABC");
    }

    // 3. Quotes and backslashes are escaped.
    {
        assert(esc("WB\"708\\165") == "WB\\\"708\\\\165");
    }

    // 4. Printable ASCII passes through unchanged.
    {
        assert(esc("WB708165 00:3c:84:fe:8f:8c") == "WB708165 00:3c:84:fe:8f:8c");
    }

    // 5. Valid UTF-8 multi-byte sequences (2, 3, 4 bytes) are preserved.
    {
        // Nordic "ÅØÆ" (2-byte), Euro "€" (3-byte), Lightning "⚡" (3-byte), Car "🚗" (4-byte)
        std::string utf8 = "\xC3\x85\xC3\x98\xC3\x86 \xE2\x82\xAC \xF0\x9F\x9A\x97";
        assert(esc(utf8) == utf8);
    }

    // 6. Invalid / truncated / overlong / surrogate UTF-8 bytes are dropped.
    {
        // Stray 0xFF, stray continuation 0x80, overlong 0xC0 0xAF, surrogate ED A0 80, truncated 2-byte C3
        std::string bad = "ok";
        bad.push_back(static_cast<char>(0xFF));
        bad.push_back(static_cast<char>(0x80));
        bad += "\xC0\xAF";
        bad += "\xED\xA0\x80";
        bad += "!";
        bad.push_back(static_cast<char>(0xC3));
        assert(esc(bad) == "ok!");
    }

    // 7. Empty and null inputs.
    {
        std::string out;
        wb_json_esc::appendEscaped(nullptr, 5, out);
        assert(out.empty());
        assert(esc("") == "");
    }

    std::printf("test_json_esc: all passed\n");
    return 0;
}
