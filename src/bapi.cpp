#include "bapi.h"
#include <utility>  // std::move for ResponseParser::takeBuffer

namespace bapi {

// ---- BAPI framing ----
// Wire format: b"EaE" + 1-byte length + payload + 1-byte checksum
// If payload >= 256 bytes: b"EaE" + 0x00 + ascii_length + 0x00 + payload + checksum

static uint8_t calcChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum & 0xFF;
}

String frame(const String& json_payload) {
    size_t plen = json_payload.length();
    String out;
    // Pre-reserve so the += chain doesn't realloc — frame() is on
    // the BAPI write path which is hot under sustained polling.
    // Worst case: 3-byte EaE + 4-byte ASCII length frame + payload +
    // 1-byte checksum. payload typically < 256 bytes; reserve gives
    // headroom for the longer s_sch / s_ecos write payloads.
    out.reserve(plen + 16);

    // EaE prefix
    out += 'E';
    out += 'a';
    out += 'E';

    if (plen < 256) {
        out += (char)plen;
    } else {
        out += (char)0;
        out += String(plen);
        out += (char)0;
    }

    out += json_payload;

    // Checksum over everything before this byte
    uint8_t cs = calcChecksum((const uint8_t*)out.c_str(), out.length());
    out += (char)cs;

    return out;
}

String buildCmd(const char* met, const char* par, int id) {
    // Build minimal JSON: {"met":"xxx","par":yyy,"id":n}
    // Pre-reserve to avoid the realloc chain across 6 += operations.
    // 128 bytes covers every BAPI command with comfortable headroom;
    // even s_sch with a full schedule body fits.
    String json;
    json.reserve(128);
    json += "{\"met\":\"";
    json += met;
    json += "\",\"par\":";
    json += par;  // already JSON (null, 32, {"pin":"1234"}, etc.)
    json += ",\"id\":";
    json += String(id);
    json += "}";
    return json;
}

// ---- Response parser ----
// Responses are raw JSON (no EaE framing). May arrive in multiple
// BLE notifications (20-byte MTU chunks).

void ResponseParser::reset() {
    _buf = "";
    // Pre-reserve so feed()'s `_buf += c` chain doesn't trigger
    // grow-and-copy reallocations during BLE notification handling.
    // Each notification is 20 bytes and a full response is typically
    // 100-3000 bytes (sometimes larger for r_log). Without reserve,
    // a 3 KB response triggers ~6 reallocations on the NimBLE host
    // task while web/MQTT tasks may be allocating concurrently —
    // exactly the heap-fragmentation race that caused the /info OOM.
    _buf.reserve(kReserveBytes);
    _braceDepth = 0;
    _inString = false;
    _escape = false;
}

String ResponseParser::takeBuffer() {
    // Move semantics: hands ownership of the accumulated buffer to
    // the caller. After this call, _buf is reset to default
    // (empty, no capacity); the NEXT reset() will reserve again.
    // Eliminates the copy that used to happen at the call site
    // (`_lastResponse = _parser.json()` was a full String copy).
    String taken = std::move(_buf);
    _buf = String();  // make sure subsequent json() / take returns empty
    _braceDepth = 0;
    _inString = false;
    _escape = false;
    return taken;  // RVO completes the move out
}

bool ResponseParser::feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        _buf += c;

        if (_escape) {
            _escape = false;
            continue;
        }
        if (c == '\\' && _inString) {
            _escape = true;
            continue;
        }
        if (c == '"') {
            _inString = !_inString;
            continue;
        }
        if (_inString) continue;

        if (c == '{') _braceDepth++;
        else if (c == '}') {
            _braceDepth--;
            if (_braceDepth == 0 && _buf.length() > 0) {
                return true;  // complete JSON object
            }
        }
    }
    return false;
}

int ResponseParser::responseId() const {
    JsonDocument doc;
    if (deserializeJson(doc, _buf) != DeserializationError::Ok) return -1;
    return doc["id"] | -1;
}

bool ResponseParser::isError() const {
    JsonDocument doc;
    if (deserializeJson(doc, _buf) != DeserializationError::Ok) return false;
    return doc.containsKey("error");
}

} // namespace bapi
