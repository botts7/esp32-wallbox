#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// BAPI framing: EaE + length + JSON payload + checksum
// Responses come back as raw JSON (no EaE framing).

namespace bapi {

// Frame a JSON payload into BAPI wire format
// Returns the framed bytes ready to write to BLE characteristic
String frame(const String& json_payload);

// Build a BAPI command JSON string
// met: method name (e.g. "r_dat", "w_cha", "ping")
// par: parameter value as JSON string (e.g. "null", "32", "{\"pin\":\"1234\"}")
// id:  request ID for matching responses
String buildCmd(const char* met, const char* par = "null", int id = 0);

// Parse a raw response (may arrive in chunks)
// Accumulates data, returns true when a complete JSON object is ready
class ResponseParser {
public:
    void reset();
    // Feed raw BLE notification data. Returns true if complete JSON ready.
    bool feed(const uint8_t* data, size_t len);
    // Get the parsed JSON string (valid after feed() returns true)
    const String& json() const { return _buf; }
    // Get response ID (-1 if not found)
    int responseId() const;
    // Check if response is an error
    bool isError() const;
private:
    String _buf;
    int _braceDepth = 0;
    bool _inString = false;
    bool _escape = false;
};

// ---- BAPI Method Names (discovered via BLE protocol analysis) ----
// Read methods
constexpr const char* MET_PING           = "ping";
constexpr const char* MET_GET_STATUS     = "r_dat";   // main status
constexpr const char* MET_GET_REALTIME   = "r_sta";   // realtime status
constexpr const char* MET_GET_SESSIONS   = "r_ses";   // session info
constexpr const char* MET_GET_SESSION    = "r_log";   // session log
constexpr const char* MET_GET_SCHEDULE   = "r_sch";   // schedule
constexpr const char* MET_GET_SCHEDULES  = "r_schs";  // all schedules
constexpr const char* MET_GET_LOCK       = "gulck";   // socket locking
constexpr const char* MET_GET_WIFI       = "gwsta";   // wifi status
constexpr const char* MET_GET_WIFI_NETS  = "gwnet";   // wifi networks
constexpr const char* MET_GET_TIMEZONE   = "g_tzn";   // timezone
constexpr const char* MET_GET_POWER_BOOST = "r_hsh";  // power boost (ICP)
constexpr const char* MET_GET_POWER_BOOST2 = "r_power_boost";
constexpr const char* MET_GET_POWER_SHARE = "g_psh";  // power sharing
constexpr const char* MET_GET_AUTOLOCK   = "g_alo";   // autolock config (inferred)
constexpr const char* MET_GET_OCPP       = "g_ocpp";  // OCPP config
constexpr const char* MET_GET_PHASE      = "g_phsw";  // phase switch
constexpr const char* MET_GET_GROUNDING  = "r_wel";   // grounding status
constexpr const char* MET_GET_V2H        = "g_pwi";   // vehicle-to-home
constexpr const char* MET_GET_METER      = "r_dca";   // power meter status
constexpr const char* MET_GET_NOTIFS     = "r_not";   // notifications
constexpr const char* MET_GET_GSM        = "gmsta";   // GSM status
constexpr const char* MET_GET_NETWORKS   = "gnsta";   // networks status
constexpr const char* MET_GET_MOBILE     = "gmcon";   // mobile connectivity
constexpr const char* MET_GET_PROXY      = "gpmod";   // proxy mode
constexpr const char* MET_GET_BATTERY    = "r_socr";  // battery config (inferred)
constexpr const char* MET_GET_CHARGER_VER = "r_ver";  // charger versions (inferred)
constexpr const char* MET_GET_DISCHARGE  = "r_dch";   // discharge session (inferred)
constexpr const char* MET_GET_ECO_SMART  = "g_ecos";  // eco smart (inferred)

// Write methods
constexpr const char* MET_START_STOP     = "w_cha";   // start/stop/pause/resume charging
constexpr const char* MET_SET_CURRENT    = "w_mxI";   // set max charging current
constexpr const char* MET_LOCK           = "w_lck";   // lock/unlock
constexpr const char* MET_SET_AUTOLOCK   = "s_alo";   // set autolock
constexpr const char* MET_SET_SCHEDULE   = "w_sch";   // set schedule
constexpr const char* MET_SET_ADV_SCHED  = "s_sch";   // set advanced schedules
constexpr const char* MET_DEL_SCHEDULES  = "clr_sch"; // delete schedules
constexpr const char* MET_SET_SOCKET_LOCK = "sulck";  // set socket locking
constexpr const char* MET_SET_TIME       = "Wtime";   // set time
constexpr const char* MET_SET_TIMEZONE   = "s_tzn";   // set timezone
constexpr const char* MET_SET_WIFI       = "swcon";   // set wifi
constexpr const char* MET_SET_WIFI_STATUS = "swsta";  // set wifi status
constexpr const char* MET_SET_POWER_BOOST = "w_hsh";  // set power boost
constexpr const char* MET_SET_POWER_BOOST2 = "w_power_boost";
constexpr const char* MET_SET_POWER_SHARE = "s_psh";  // set power sharing
constexpr const char* MET_CLR_POWER_SHARE = "clr_psh";// clear power sharing
constexpr const char* MET_SET_CONTROL    = "s_cmode"; // set control mode
constexpr const char* MET_SET_PHASE      = "s_phsw";  // set phase switch
constexpr const char* MET_SET_ECO_SMART  = "s_ecos";  // set eco smart
constexpr const char* MET_SET_GESTURE    = "sgsta";   // set gesture config
constexpr const char* MET_SET_HALO       = "s_halocfg"; // set LED halo config
constexpr const char* MET_SET_OCPP       = "s_ocpp";  // set OCPP
constexpr const char* MET_SET_BATTERY    = "w_socr";  // set battery config
constexpr const char* MET_SET_V2H        = "s_pwi";   // set vehicle-to-home
constexpr const char* MET_SET_MID        = "s_mid";   // set MID config
constexpr const char* MET_SET_MULTIUSER  = "s_mus";   // set multiuser
constexpr const char* MET_SET_USER       = "suser";   // set user
constexpr const char* MET_SET_USERLIST   = "sulis";   // set user list
constexpr const char* MET_SET_IP_MODE    = "simod";   // set IP mode
constexpr const char* MET_SET_GROUNDING  = "w_wel";   // set grounding
constexpr const char* MET_SET_MOBILE     = "smcon";   // set mobile connectivity
constexpr const char* MET_SET_MOBILE_EN  = "smcen";   // set mobile connectivity status
constexpr const char* MET_SET_GRID_CODE  = "w_gcd";   // set grid code
constexpr const char* MET_SET_PROXY      = "spmod";   // set proxy mode
constexpr const char* MET_REBOOT         = "rebot";   // reboot charger

// PIN methods
constexpr const char* MET_READ_PIN       = "read_pin";
constexpr const char* MET_SET_PIN        = "set_pin";

// Software update methods
constexpr const char* MET_SW_CHECK       = "gupdc";   // software check
constexpr const char* MET_SW_UPDATE      = "supds";   // start software update
constexpr const char* MET_SW_PROGRESS    = "supdp";   // update progress
constexpr const char* MET_HOTSPOT_UPDATE = "s_hup";   // hotspot update
constexpr const char* MET_CANCEL_HOTSPOT = "c_hup";   // cancel hotspot update

// Grid code methods
constexpr const char* MET_GET_GRID_CONF  = "get_grid_conf";
constexpr const char* MET_SET_GRID_CONF  = "set_grid_conf";
constexpr const char* MET_GET_GC_STATUS  = "get_gc_status";
constexpr const char* MET_SET_GC_STATUS  = "set_gc_status";
constexpr const char* MET_GET_GC_REGS    = "get_available_regs";
constexpr const char* MET_GET_GC_PARAMS  = "read_gc_params";
constexpr const char* MET_SET_GC_PARAMS  = "update_gc_params";
constexpr const char* MET_GET_GC_LOGS    = "get_gc_logs";
constexpr const char* MET_GET_GC_ALERTS  = "get_gc_alerts";

// OCPP certificate methods
constexpr const char* MET_SET_OCPP_CERT  = "scert_ocpp";
constexpr const char* MET_GET_OCPP_CERT  = "gcert_ocpp";
constexpr const char* MET_DEL_OCPP_CERT  = "dcert_ocpp";

} // namespace bapi
