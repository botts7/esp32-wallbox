#include "bapi.h"

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
    String json = "{\"met\":\"";
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
    _braceDepth = 0;
    _inString = false;
    _escape = false;
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
