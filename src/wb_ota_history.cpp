#include "wb_ota_history.h"
#include "wb_log.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace wb_ota_history {

static const char* NVS_NS = "wbota";
static const char* NVS_KEY = "hist";  // JSON array of entries

static void load(JsonDocument& doc) {
    Preferences p;
    if (!p.begin(NVS_NS, true)) {
        doc.to<JsonArray>();
        return;
    }
    String s = p.getString(NVS_KEY, "[]");
    p.end();
    if (deserializeJson(doc, s) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        doc.clear();
        doc.to<JsonArray>();
    }
}

static void store(const JsonDocument& doc) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    String s;
    serializeJson(doc, s);
    // Remove the existing key before rewriting. On a near-full / fragmented
    // NVS, growing an existing value in place fails SILENTLY — putString
    // returns 0 and the OLD value persists. That's exactly what froze this
    // box's OTA history at beta.6 while a dozen later flashes went unrecorded
    // (same silent-fail class as the charge-log store, project rc.4). Freeing
    // the page first lets the write land, and we now surface a failure instead
    // of losing it quietly.
    p.remove(NVS_KEY);
    size_t n = p.putString(NVS_KEY, s);
    if (n == 0 && s.length() > 2) {  // "[]" is 2 bytes; a real array is longer
        Log.printf("[OTAhist] NVS store FAILED (%u bytes) — history not persisted\n",
                   (unsigned)s.length());
    }
    p.end();
}

void recordOta(uint32_t uptime_s, const String& was_running,
               uint32_t size_bytes, bool success, const String& reason,
               const String& target) {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();

    // Append the new entry at the END (we'll re-emit newest-first in toJson)
    JsonObject e = arr.add<JsonObject>();
    e["kind"]     = "ota";
    e["uptime_s"] = uptime_s;
    e["version"]  = was_running;
    e["bytes"]    = size_bytes;
    e["ok"]       = success;
    e["reason"]   = reason;
    // Compatibility — older /info renderers still read "from".
    e["from"]     = was_running;
    // The version this upload INSTALLED (TO). We don't know it yet at upload
    // time — the app-descriptor version field of the incoming image is the IDF
    // version, not our git-describe WB_VERSION — so `target` is normally empty
    // here and gets backfilled by recordBoot() once the new firmware actually
    // boots (see below). The param stays for callers that already know it.
    if (target.length()) e["to"] = target;

    while ((int)arr.size() > MAX_ENTRIES) arr.remove(0);
    store(doc);
}

void recordBoot(uint32_t uptime_s, const String& version) {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();

    // Backfill the TO version onto the OTA that installed us. This boot is the
    // ground truth for "what did that upload actually install" — WB_VERSION
    // here is our real git-describe string, unlike the image's app-descriptor
    // version (which is the IDF version). Walk back from the newest entry to
    // the first successful `ota` that has no `to` yet and stamp it. Stop at any
    // earlier boot entry — an ota before a previous boot belongs to that boot,
    // not this one. peter-mcc #13: "shows beta.14, not the rc.4 it got me to".
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
        JsonObject o = arr[i];
        const char* k = o["kind"].is<const char*>() ? o["kind"].as<const char*>() : "";
        if (String(k) == "boot") break;                 // reached the prior boot; done
        if (String(k) == "ota" && o["ok"].as<bool>() && !o["to"].is<const char*>()) {
            o["to"] = version;
            break;
        }
    }

    // Deduplicate — if the most recent entry is a boot of the same
    // version, don't record again. Avoids duplicate entries from
    // multiple markHealthy() calls or quick reboots of the same FW.
    // (Still store() first so a backfill above isn't lost on the dedup path.)
    if (arr.size() > 0) {
        JsonObject last = arr[arr.size() - 1];
        if (last["kind"].is<const char*>()
            && String(last["kind"].as<const char*>()) == "boot"
            && last["version"].is<const char*>()
            && String(last["version"].as<const char*>()) == version) {
            store(doc);
            return;
        }
    }

    JsonObject e = arr.add<JsonObject>();
    e["kind"]     = "boot";
    e["uptime_s"] = uptime_s;
    e["version"]  = version;
    e["ok"]       = true;

    while ((int)arr.size() > MAX_ENTRIES) arr.remove(0);
    store(doc);
}

String toJson() {
    JsonDocument doc;
    load(doc);
    JsonArray arr = doc.as<JsonArray>();

    // Emit newest-first by reversing
    JsonDocument out;
    JsonArray oa = out.to<JsonArray>();
    for (int i = (int)arr.size() - 1; i >= 0; i--) {
        oa.add(arr[i]);
    }
    String s;
    serializeJson(out, s);
    return s;
}

}  // namespace wb_ota_history
