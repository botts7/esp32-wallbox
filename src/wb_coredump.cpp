#include "wb_coredump.h"

#include "wb_log.h"
#include "wb_web.h"  // wb_jsonEsc — exc_task comes from a corrupted-crash
                     // context and must not be trusted to be clean JSON.

#include <esp_core_dump.h>
#include <esp_partition.h>

namespace wb_coredump {

// The coredump partition, looked up once. Null on any install whose table
// predates it (OTA-only devices) — every entry point below degrades to
// "no dump" rather than failing.
static const esp_partition_t* _part() {
    static const esp_partition_t* p = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                     nullptr);
        if (!p) Log.println("[Coredump] no coredump partition (pre-#168 table)");
    }
    return p;
}

bool partitionAvailable() { return _part() != nullptr; }

bool present() {
    if (!_part()) return false;
    size_t addr = 0, sz = 0;
    // image_get() validates the header; image_check() validates the CRC. A
    // dump that fails the CRC is a half-written crash (we reset mid-dump) —
    // report it as absent rather than hand out bytes that decode to fiction.
    if (esp_core_dump_image_get(&addr, &sz) != ESP_OK || sz == 0) return false;
    return esp_core_dump_image_check() == ESP_OK;
}

size_t size() {
    if (!_part()) return 0;
    size_t addr = 0, sz = 0;
    if (esp_core_dump_image_get(&addr, &sz) != ESP_OK) return 0;
    return sz;
}

size_t readChunk(size_t offset, uint8_t* buf, size_t len) {
    const esp_partition_t* p = _part();
    if (!p || !buf || !len) return 0;
    size_t total = size();
    if (offset >= total) return 0;
    size_t n = (total - offset < len) ? (total - offset) : len;
    // Read partition-relative: esp_core_dump_image_get()'s address is an
    // absolute flash offset, but esp_partition_read() wants an offset within
    // the partition, and the image always starts at its base.
    if (esp_partition_read(p, offset, buf, n) != ESP_OK) return 0;
    return n;
}

bool erase() {
    if (!_part()) return false;
    esp_err_t e = esp_core_dump_image_erase();
    Log.printf("[Coredump] erase -> %s\n", esp_err_to_name(e));
    return e == ESP_OK;
}

String summaryJson() {
    // Distinguish "this device can't ever capture a crash" from "no crash
    // yet" — see partitionAvailable(). Reporting both as {"present":false}
    // is how you spend a week waiting for a dump that was never going to
    // arrive.
    if (!partitionAvailable()) {
        return String("{\"present\":false,\"partition\":false,\"note\":"
                      "\"no coredump partition - USB re-flash required to "
                      "enable crash capture on this device\"}");
    }
    if (!present()) return String("{\"present\":false,\"partition\":true}");

    esp_core_dump_summary_t s;
    if (esp_core_dump_get_summary(&s) != ESP_OK) {
        // A dump exists but won't summarise (older format / truncated). The
        // raw download still works, so say so rather than claim there's
        // nothing here.
        String j = "{\"present\":true,\"partition\":true,\"summary\":false,\"size\":";
        j += String((unsigned)size());
        j += ",\"note\":\"image present but not summarisable - fetch /api/coredump\"}";
        return j;
    }

    String j;
    j.reserve(768);
    j += "{\"present\":true,\"partition\":true,\"summary\":true";
    j += ",\"size\":";      j += String((unsigned)size());
    // The whole point of #168: which task actually died.
    j += ",\"task\":\"";    j += wb_jsonEsc(String(s.exc_task)); j += "\"";
    j += ",\"pc\":";        j += String((unsigned)s.exc_pc);
    j += ",\"pc_hex\":\"0x"; {
        char b[9]; snprintf(b, sizeof(b), "%08x", (unsigned)s.exc_pc); j += b;
    } j += "\"";
    j += ",\"exc_cause\":"; j += String((unsigned)s.ex_info.exc_cause);
    j += ",\"exc_vaddr\":"; j += String((unsigned)s.ex_info.exc_vaddr);
    j += ",\"dump_ver\":";  j += String((unsigned)s.core_dump_version);

    // Backtrace PCs. Symbolise with:
    //   xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <pc> <pc> ...
    j += ",\"bt_corrupted\":";
    j += s.exc_bt_info.corrupted ? "true" : "false";
    j += ",\"backtrace\":[";
    uint32_t depth = s.exc_bt_info.depth;
    if (depth > 16) depth = 16;  // struct is fixed at bt[16]; never over-read
    for (uint32_t i = 0; i < depth; i++) {
        if (i) j += ",";
        char b[13];
        snprintf(b, sizeof(b), "\"0x%08x\"", (unsigned)s.exc_bt_info.bt[i]);
        j += b;
    }
    j += "]";

    // Which ELF this dump belongs to. Symbolising against the wrong build
    // produces a confident, wrong stack — worse than none. Compare against
    // the archived firmware.elf before believing any line number.
    j += ",\"app_elf_sha256\":\"";
    for (size_t i = 0; i < sizeof(s.app_elf_sha256); i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", s.app_elf_sha256[i]);
        j += b;
        if (j.length() > 700) break;  // hard stop; the field is display-only
    }
    j += "\"}";
    return j;
}

}  // namespace wb_coredump
