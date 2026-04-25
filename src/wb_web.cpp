#include "wb_web.h"
#include "wb_config.h"
#include "wb_ble.h"
#include "bapi.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <esp_ota_ops.h>
WBWebServer webServer;

static WebServer http(80);
static DNSServer dns;
static const char* AP_SSID = "WallboxGW-Setup";
static const char* AP_PASS = "wallbox123";

// ========== CSRF Token ==========
// Generated at boot, validated on all state-changing POST endpoints
static String csrfToken;
static void ensureCsrfToken() {
    if (csrfToken.length() == 0) {
        uint8_t mac[6]; WiFi.macAddress(mac);
        uint32_t seed = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                        ((uint32_t)mac[4] << 8) | mac[5];
        seed ^= millis() ^ ESP.getCycleCount();
        randomSeed(seed);
        csrfToken = "";
        for (int i = 0; i < 16; i++) {
            char c[3];
            snprintf(c, 3, "%02x", (int)random(0, 256));
            csrfToken += c;
        }
    }
}
static bool checkCsrf() {
    ensureCsrfToken();
    String token = http.arg("csrf");
    if (token.length() == 0 || token != csrfToken) {
        http.send(403, "text/plain", "CSRF token mismatch");
        return false;
    }
    return true;
}

// ========== Web Authentication ==========
static uint32_t authFailCount = 0;
static uint32_t authLockoutUntil = 0;

static bool checkAuth() {
    const WBConfig& cfg = configMgr.get();
    if (!cfg.authEnabled || cfg.authPass.length() == 0) return true;

    // Rate limiting — lockout after 5 failures for 30s
    if (authFailCount >= 5 && millis() < authLockoutUntil) {
        http.send(429, "text/plain", "Too many attempts. Try again in 30 seconds.");
        return false;
    }
    if (millis() >= authLockoutUntil) authFailCount = 0;

    if (!http.authenticate(cfg.authUser.c_str(), cfg.authPass.c_str())) {
        authFailCount++;
        if (authFailCount >= 5) {
            authLockoutUntil = millis() + 30000;
            Serial.printf("[Auth] LOCKED OUT after %d failures\n", authFailCount);
        } else {
            delay(1000);  // 1s delay per failure — slows brute force
            Serial.printf("[Auth] Failed %d/5\n", authFailCount);
        }
        http.requestAuthentication();
        return false;
    }
    authFailCount = 0;
    return true;
}

// ========== CSS ==========
static void handleStyleCss() {
    http.sendHeader("Cache-Control", "no-cache");
    http.send(200, "text/css", R"CSS(
:root{--bg:#0f1117;--surface:#1a1d28;--elevated:#232736;--primary:#3b82f6;--success:#22c55e;--danger:#ef4444;--warning:#f59e0b;--text:#e2e8f0;--text2:#94a3b8;--text3:#64748b;--border:#2a2d3a;--accent:#4fc3f7}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:16px;padding-bottom:80px;background:var(--bg);color:var(--text);min-height:100vh;-webkit-font-smoothing:antialiased}
.container{max-width:600px;margin:0 auto}
h1{color:var(--text);font-size:1.4em;font-weight:700;margin-top:8px;letter-spacing:-.02em}
h2{font-size:1.05em;color:var(--text2);font-weight:600;margin:0 0 12px 0}
.subtitle{color:var(--text3);font-size:.82em;margin-top:2px}
.card{background:var(--surface);padding:20px;border-radius:14px;margin:12px 0;box-shadow:0 4px 20px rgba(0,0,0,.25);border:1px solid var(--border)}
.card-header{display:flex;align-items:center;gap:8px;margin-bottom:14px}
.card-icon{font-size:1.3em}
label{font-weight:500;color:var(--text2);font-size:.85em;display:block;margin-bottom:4px}
input,select{width:100%;padding:12px;margin:0 0 12px 0;border:1px solid var(--border);border-radius:10px;background:var(--elevated);color:var(--text);font-size:15px;transition:border-color .2s,box-shadow .2s}
input:focus,select:focus{border-color:var(--primary);outline:none;box-shadow:0 0 0 3px rgba(59,130,246,.15)}
input::placeholder{color:var(--text3)}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.btn{display:block;width:100%;box-sizing:border-box;background:var(--primary);color:#fff;padding:14px;border:1px solid transparent;border-radius:10px;font-size:15px;font-weight:600;line-height:1.2;cursor:pointer;transition:all .2s;box-shadow:0 2px 8px rgba(59,130,246,.25);text-align:center;text-decoration:none}
a.btn{text-decoration:none}
.btn:hover{filter:brightness(1.1);transform:translateY(-1px)}
.btn:active{transform:translateY(0)}
.btn-success{background:linear-gradient(135deg,#22c55e,#16a34a);box-shadow:0 2px 8px rgba(34,197,94,.25)}
.btn-danger{background:linear-gradient(135deg,#ef4444,#dc2626);box-shadow:0 2px 8px rgba(239,68,68,.25)}
.btn-outline{background:transparent;border:1px solid var(--border);color:var(--text2);box-shadow:none}
.btn-outline:hover{border-color:var(--primary);color:var(--primary)}
.btn-small{padding:10px 16px;font-size:13px;border-radius:8px;width:auto;display:inline-block}
.status-bar{background:var(--surface);border-radius:10px;padding:12px 16px;margin:10px 0;border:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:6px}
.status-item{font-size:.82em}
.status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
.dot-green{background:var(--success)}.dot-red{background:var(--danger)}.dot-yellow{background:var(--warning)}
.scan-result{background:var(--elevated);padding:12px 16px;border-radius:10px;margin:6px 0;border:1px solid var(--border);cursor:pointer;transition:all .2s;display:flex;justify-content:space-between;align-items:center}
.scan-result:hover{border-color:var(--primary)}.scan-result.wb{border-color:var(--success);background:rgba(34,197,94,.06)}
.scan-name{font-weight:600;font-size:.9em}.scan-addr{color:var(--text3);font-size:.8em;font-family:monospace}.scan-rssi{color:var(--text3);font-size:.78em}
.badge{display:inline-block;padding:2px 8px;border-radius:20px;font-size:.72em;font-weight:600;margin-left:6px}
.badge-success{background:rgba(34,197,94,.15);color:var(--success)}.badge-warning{background:rgba(245,158,11,.15);color:var(--warning)}
.spinner{display:inline-block;width:16px;height:16px;border:2px solid var(--primary);border-top:2px solid transparent;border-radius:50%;animation:spin .8s linear infinite;margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.help{color:var(--text3);font-size:.8em;margin-top:-6px;margin-bottom:10px}
.divider{border:none;border-top:1px solid var(--border);margin:14px 0}
input[type=range]{-webkit-appearance:none;appearance:none;height:6px;border-radius:3px;background:var(--border);padding:0;margin:8px 0}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:var(--primary);cursor:pointer;border:2px solid var(--bg)}
input[type=range]::-moz-range-thumb{width:22px;height:22px;border-radius:50%;background:var(--primary);cursor:pointer;border:2px solid var(--bg)}
input[type=range]::-webkit-slider-runnable-track{border-radius:3px}
.ctrl-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.ctrl-grid .btn{height:54px;padding:0;margin:0;font-size:14px;box-shadow:none}
.info-row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)}.info-row:last-child{border:none}
.info-label{color:var(--text2);font-size:.88em}.info-value{font-weight:500;font-size:.88em}
.nav-bar{position:fixed;bottom:0;left:0;right:0;display:flex;background:rgba(15,17,23,.92);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);border-top:1px solid var(--border);padding:6px 0 env(safe-area-inset-bottom,8px);z-index:100}
.nav-item{flex:1 1 0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;text-decoration:none;color:var(--text3);font-size:.7em;font-weight:500;padding:6px 4px;transition:color .2s;min-width:0}
.nav-item.active{color:var(--primary)}
.nav-item svg{width:22px;height:22px;fill:currentColor}
)CSS");
}

// ========== JS (shared) ==========
static void handleAppJs() {
    http.sendHeader("Cache-Control", "no-cache");
    http.send(200, "application/javascript", R"JS(
function toast(msg,type){type=type||'info';var c=document.getElementById('toast-c');if(!c){c=document.createElement('div');c.id='toast-c';c.className='toast-container';document.body.appendChild(c)}var t=document.createElement('div');t.className='toast toast-'+type;t.textContent=msg;c.appendChild(t);setTimeout(function(){t.style.opacity='0';t.style.transition='opacity .3s';setTimeout(function(){t.remove()},300)},3000)}
function confirm2(msg,cb){var o=document.createElement('div');o.style.cssText='position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:300;display:flex;align-items:center;justify-content:center';o.innerHTML="<div style='background:#1a1d28;border:1px solid #2a2d3a;border-radius:14px;padding:24px;max-width:320px;text-align:center'><p style='margin:0 0 16px;color:#e2e8f0'>"+msg+"</p><div style='display:flex;gap:10px'><button style='flex:1;padding:12px;border-radius:8px;border:1px solid #2a2d3a;background:transparent;color:#94a3b8;cursor:pointer' onclick='this.closest(\"div[style]\").remove()'>Cancel</button><button style='flex:1;padding:12px;border-radius:8px;border:none;background:#ef4444;color:#fff;cursor:pointer' id='cf-ok'>Confirm</button></div></div>";document.body.appendChild(o);document.getElementById('cf-ok').onclick=function(){o.remove();cb()};o.onclick=function(e){if(e.target===o)o.remove()}}
function selectDevice(addr){document.getElementById('ble_addr').value=addr;document.querySelectorAll('.scan-result').forEach(function(e){e.style.borderColor=''});event.currentTarget.style.borderColor='var(--primary)'}
function formatMAC(i){var v=i.value.replace(/[^0-9a-fA-F]/g,'').toUpperCase(),f='';for(var j=0;j<v.length&&j<12;j++){if(j>0&&j%2===0)f+=':';f+=v[j]}i.value=f}
function startScan(){var b=document.getElementById('scanBtn'),r=document.getElementById('scanResults');b.disabled=true;b.innerHTML="<span class='spinner'></span>Scanning...";r.innerHTML="<div style='text-align:center;padding:16px;color:var(--text3)'><span class='spinner'></span> Scanning...</div>";fetch('/api/ble-scan').then(function(x){return x.json()}).then(function(d){b.disabled=false;b.innerHTML='Scan for Chargers';if(!d.devices.length){r.innerHTML="<div style='text-align:center;padding:16px;color:var(--text3)'>No devices found</div>";return}var h='';d.devices.forEach(function(v){var c=v.is_wallbox?'scan-result wb':'scan-result';var bg=v.is_wallbox?"<span class='badge badge-success'>WALLBOX</span>":'';h+="<div class='"+c+"' onclick=\"selectDevice('"+v.addr+"')\"><div><span class='scan-name'>"+(v.name||'Unknown')+"</span>"+bg+"<br><span class='scan-addr'>"+v.addr+"</span></div><span class='scan-rssi'>"+v.rssi+" dBm</span></div>"});r.innerHTML=h}).catch(function(e){b.disabled=false;b.innerHTML='Scan for Chargers';r.innerHTML="<div style='color:var(--danger)'>"+e+"</div>"})}
function pickSsid(s){var i=document.getElementById('wifi_ssid');if(i)i.value=s;var r=document.getElementById('wifi-results');if(r)r.style.display='none';toast('Selected: '+s,'info')}
function scanWifi(){
  var b=document.getElementById('wifiScanBtn');
  var r=document.getElementById('wifi-results');
  if(!b||!r){toast('Scan UI not ready — refresh page','error');return}
  b.disabled=true;b.innerHTML="<span class='spinner'></span>";
  r.style.display='block';
  r.innerHTML="<div style='padding:10px;text-align:center;color:var(--text3)'><span class='spinner'></span> Scanning WiFi...</div>";
  fetch('/api/wifi-scan',{signal:AbortSignal.timeout(20000)}).then(function(x){return x.json()}).then(function(d){
    b.disabled=false;b.innerHTML='Scan';
    if(d.error){r.innerHTML="<div style='padding:10px;color:var(--danger)'>Scan error: "+d.error+"</div>";return}
    if(!d.networks||!d.networks.length){r.innerHTML="<div style='padding:10px;text-align:center;color:var(--text3)'>No networks found</div>";return}
    var o='';d.networks.sort(function(a,b){return b.rssi-a.rssi}).forEach(function(n){
      var ss=n.ssid.replace(/'/g,"\\'");
      o+="<div onclick=\"pickSsid('"+ss+"')\" style='padding:10px 12px;border-radius:6px;cursor:pointer;display:flex;justify-content:space-between;align-items:center'>";
      o+="<span style='font-weight:500'>"+n.ssid+"</span>";
      o+="<span style='color:var(--text3);font-size:.8em'>"+n.rssi+" dBm</span>";
      o+="</div>";
    });
    r.innerHTML=o;
    toast('Found '+d.networks.length+' networks','success');
  }).catch(function(e){
    b.disabled=false;b.innerHTML='Scan';
    if(r)r.innerHTML="<div style='padding:10px;color:var(--danger)'>Scan failed: "+(e.message||e)+"</div>";
    toast('Scan failed','error');
  });
}
function row(l,v){return "<div class='info-row'><span class='info-label'>"+l+"</span><span class='info-value'>"+v+"</span></div>"}
)JS");
}

// ========== HTML helpers ==========

// SVG icons for nav (inline, no external deps)
#define SVG_DASHBOARD "<svg viewBox='0 0 24 24'><path d='M3 13h8V3H3v10zm0 8h8v-6H3v6zm10 0h8V11h-8v10zm0-18v6h8V3h-8z'/></svg>"
#define SVG_SETTINGS  "<svg viewBox='0 0 24 24'><path d='M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.07.62-.07.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z'/></svg>"
#define SVG_CONFIG    "<svg viewBox='0 0 24 24'><path d='M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z'/></svg>"
#define SVG_INFO      "<svg viewBox='0 0 24 24'><path d='M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z'/></svg>"

static String htmlHead(const char* title = "Wallbox Gateway") {
    bool bleOk = wallboxBLE.isConnected();
    String bleState = wallboxBLE.stateStr();
    int rssi = wallboxBLE.rssi();

    String h = "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta name='apple-mobile-web-app-capable' content='yes'>"
        "<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>"
        "<meta name='mobile-web-app-capable' content='yes'>"
        "<meta name='theme-color' content='#0f1117'>"
        "<link rel='manifest' href='/manifest.json'>"
        "<title>";
    h += title;
    h += "</title>"
        "<style>"
        "body{margin:0;padding:16px;padding-bottom:80px;background:#0f1117;color:#e2e8f0;font-family:-apple-system,system-ui,sans-serif;min-height:100vh}"
        ".container{max-width:600px;margin:0 auto}"
        ".loading{display:flex;flex-direction:column;align-items:center;justify-content:center;padding:60px 0;color:#64748b}"
        ".ld-spin{width:32px;height:32px;border:3px solid #3b82f6;border-top:3px solid transparent;border-radius:50%;animation:sp .8s linear infinite;margin-bottom:12px}"
        "@keyframes sp{to{transform:rotate(360deg)}}"
        ".ble-bar{display:flex;align-items:center;gap:6px;padding:8px 14px;border-radius:8px;font-size:.78em;font-weight:500;margin-bottom:8px;"
        "background:";
    h += bleOk ? "rgba(34,197,94,.08);border:1px solid rgba(34,197,94,.2);color:#22c55e"
               : "rgba(239,68,68,.08);border:1px solid rgba(239,68,68,.2);color:#ef4444";
    h += "}.ble-dot{width:6px;height:6px;border-radius:50%;background:currentColor}"
        ".nav-bar{position:fixed;bottom:0;left:0;right:0;display:flex;background:#0f1117;border-top:1px solid #2a2d3a;padding:6px 0 8px;z-index:100}"
        ".nav-item{flex:1 1 0;display:flex;flex-direction:column;align-items:center;justify-content:center;text-decoration:none;color:#64748b;font-size:.7em;padding:6px 4px;min-width:0}"
        ".nav-item.active{color:#3b82f6}"
        ".toast-container{position:fixed;top:16px;left:50%;transform:translateX(-50%);z-index:200;display:flex;flex-direction:column;gap:8px;pointer-events:none;width:90%;max-width:400px}"
        ".toast{padding:12px 16px;border-radius:10px;font-size:.88em;font-weight:500;pointer-events:auto;animation:toastIn .3s ease;box-shadow:0 4px 20px rgba(0,0,0,.4)}"
        ".toast-success{background:#16a34a;color:#fff}"
        ".toast-error{background:#dc2626;color:#fff}"
        ".toast-info{background:#2563eb;color:#fff}"
        "@keyframes toastIn{from{opacity:0;transform:translateY(-10px)}to{opacity:1;transform:translateY(0)}}"
        "</style>"
        "</head><body><div class='container'>"
        "<div class='ble-bar'><span class='ble-dot'></span>BLE: ";
    h += bleState;
    if (bleOk && rssi > -127) {
        h += " (" + String(rssi) + " dBm)";
    }
    h += "</div>";
    return h;
}

static String htmlFoot(const char* activePath) {
    // Cache-bust CSS/JS with boot time (unique per firmware build + boot)
    static String buildVer;
    if (buildVer.length() == 0) {
        buildVer = String((uint32_t)(esp_random() & 0xFFFFFF), HEX);
    }
    String h = "<link rel='stylesheet' href='/style.css?v=" + buildVer + "'>"
               "<script src='/app.js?v=" + buildVer + "'></script>"
               "</div><nav class='nav-bar'>";
    auto navItem = [&](const char* href, const char* svg, const char* label) {
        h += "<a href='";
        h += href;
        h += "' class='nav-item";
        if (strcmp(activePath, href) == 0) h += " active";
        h += "'>";
        h += svg;
        h += "<span>";
        h += label;
        h += "</span></a>";
    };
    navItem("/", SVG_DASHBOARD, "Dashboard");
    navItem("/settings", SVG_SETTINGS, "Settings");
    navItem("/config", SVG_CONFIG, "Config");
    navItem("/info", SVG_INFO, "Info");
    h += "</nav>"
         "<script>if('serviceWorker' in navigator)navigator.serviceWorker.register('/sw.js').catch(function(){});</script>"
         "</body></html>";
    return h;
}

// ========== API endpoints ==========

static void handleBleScan() {
    // Don't scan if BLE is actively connecting — causes conflicts
    if (wallboxBLE.state() == WallboxBLE::State::CONNECTING ||
        wallboxBLE.state() == WallboxBLE::State::AUTHENTICATING) {
        http.send(200, "application/json", "{\"devices\":[],\"busy\":true}");
        return;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults results = scan->start(8, false);
    String json = "{\"devices\":[";
    bool first = true;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        String name = dev.getName().c_str();
        String addr = dev.getAddress().toString().c_str();
        int rssi = dev.getRSSI();
        bool isWB = name.startsWith("WB") || name.indexOf("allbox") >= 0;
        if (!first) json += ",";
        first = false;
        json += "{\"addr\":\"" + addr + "\",\"name\":\"" + name + "\",\"rssi\":" + String(rssi) + ",\"is_wallbox\":" + (isWB ? "true" : "false") + "}";
    }
    json += "]}";
    http.send(200, "application/json", json);
}

static void handleWifiScan() {
    // Ensure STA is enabled for scanning (AP-only mode can't scan)
    wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    else if (mode == WIFI_OFF) WiFi.mode(WIFI_STA);

    int n = WiFi.scanNetworks(false, true, false, 400);
    if (n < 0) {
        http.send(500, "application/json", "{\"error\":\"scan failed\",\"code\":" + String(n) + "}");
        return;
    }
    String json = "{\"networks\":[";
    bool first = true;
    for (int i = 0; i < n && i < 20; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;  // hidden networks
        if (!first) json += ",";
        first = false;
        // Escape quotes in SSID
        String s = ssid; s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + s + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]}";
    WiFi.scanDelete();
    http.send(200, "application/json", json);
}

static void handleApiStatus() {
    String json = "{";
    json += "\"wifi\":\"" + String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") + "\"";
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"ssid\":\"" + WiFi.SSID() + "\"";
    json += ",\"wifi_rssi\":" + String(WiFi.RSSI());
    json += ",\"ble\":\"" + String(wallboxBLE.stateStr()) + "\"";
    json += ",\"rssi\":" + String(wallboxBLE.rssi());
    json += ",\"scan_rssi\":" + String(wallboxBLE.scanRSSI());
    json += ",\"tx\":" + String(wallboxBLE.txCount());
    json += ",\"rx\":" + String(wallboxBLE.rxCount());
    json += ",\"uptime\":" + String(millis() / 1000);
    json += ",\"heap\":" + String(ESP.getFreeHeap());
    json += ",\"dev_mfg\":\"" + wallboxBLE.deviceManufacturer() + "\"";
    json += ",\"dev_model\":\"" + wallboxBLE.deviceModel() + "\"";
    json += ",\"dev_fw\":\"" + wallboxBLE.deviceFirmware() + "\"";
    json += ",\"dev_name\":\"" + wallboxBLE.deviceName() + "\"";
    json += "}";
    http.send(200, "application/json", json);
}

// Cache last charger data so web UI never blocks on BLE
static String _cachedStatus = "null";
static String _cachedRealtime = "null";
static uint32_t _cacheTime = 0;

void WBWebServer::updateCache(const String& status, const String& realtime) {
    if (!status.isEmpty()) _cachedStatus = status;
    if (!realtime.isEmpty()) _cachedRealtime = realtime;
    _cacheTime = millis();
}

static void handleApiCharger() {
    // Always return cached data instantly — never block on BLE
    String json = "{\"status\":" + _cachedStatus +
                  ",\"realtime\":" + _cachedRealtime +
                  ",\"ble\":\"" + String(wallboxBLE.stateStr()) + "\"" +
                  ",\"cache_age\":" + String((millis() - _cacheTime) / 1000) + "}";
    http.send(200, "application/json", json);
}

static void handleApiCommand() {
    if (!checkAuth()) return;
    if (!wallboxBLE.isConnected()) {
        http.send(503, "application/json", "{\"error\":\"BLE not connected\"}");
        return;
    }
    String action = http.arg("action");
    String value = http.arg("value");
    String resp;
    if (action == "start") resp = wallboxBLE.sendCommand(bapi::MET_START_STOP, "1");
    else if (action == "stop") resp = wallboxBLE.sendCommand(bapi::MET_START_STOP, "2");
    else if (action == "lock") resp = wallboxBLE.sendCommand(bapi::MET_LOCK, "1");
    else if (action == "unlock") resp = wallboxBLE.sendCommand(bapi::MET_LOCK, "0");
    else if (action == "current") resp = wallboxBLE.sendCommand(bapi::MET_SET_CURRENT, value.c_str());
    else if (action == "reboot") resp = wallboxBLE.sendCommand(bapi::MET_REBOOT, "null");
    else if (action == "bapi") {
        String met = http.arg("met");
        String par = http.arg("par");
        if (par.isEmpty()) par = "null";
        resp = wallboxBLE.sendCommand(met.c_str(), par.c_str());
    } else {
        http.send(400, "application/json", "{\"error\":\"unknown action\"}");
        return;
    }
    http.send(200, "application/json", resp.isEmpty() ? "{\"error\":\"timeout\"}" : resp);
}

static String normalizeMAC(const String& raw) {
    String hex;
    for (size_t i = 0; i < raw.length(); i++) {
        char c = raw[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) hex += c;
    }
    hex.toUpperCase();
    if (hex.length() != 12) return raw;
    String out;
    for (int i = 0; i < 12; i++) {
        if (i > 0 && i % 2 == 0) out += ':';
        out += hex[i];
    }
    return out;
}

// ========== PAGE 1: Dashboard (/) ==========
static void handleDashboard() {
    String page = htmlHead("Dashboard");
    page += R"HTML(
<div class='loading' id='ld'><div class='ld-spin'></div>Loading Dashboard...</div>
<div id='pg' style='display:none'>
<h1>&#x26A1; Wallbox</h1>
<div id='pin-warn' style='display:none;background:rgba(245,158,11,.08);border:1px solid rgba(245,158,11,.2);border-radius:8px;padding:10px;margin-bottom:10px;font-size:.82em;color:#f59e0b'>&#x26A0; No BLE PIN set — anyone nearby can control your charger. <a href='/settings' style='color:#f59e0b;text-decoration:underline'>Set a PIN</a></div>

<div class='card'>
  <div id='sg' style='display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px'>
    <div><label>Status</label><div id='v-st' class='info-value'>--</div></div>
    <div><label>Charging Power</label><div id='v-pw' class='info-value'>--</div></div>
    <div><label id='l-cr'>Charging Current</label><div id='v-cr' class='info-value'>--</div></div>
    <div><label>Session Energy</label><div id='v-en' class='info-value'>--</div></div>
    <div><label>Max Current</label><div id='v-mc' class='info-value'>--</div></div>
    <div><label>Socket Lock</label><div id='v-lk' class='info-value'>--</div></div>
    <div><label>Mains Voltage</label><div id='v-vt' class='info-value'>--</div></div>
    <div><label title='Whole-house power from charger MID meter'>House Power</label><div id='v-gp' class='info-value'>--</div></div>
  </div>
</div>

<div class='card'>
  <div class='ctrl-grid'>
    <button class='btn btn-success' onclick='C("start")'>&#x25B6; Start</button>
    <button class='btn btn-danger' onclick='C("stop")'>&#x23F9; Stop</button>
    <button class='btn' onclick='C("unlock")'>&#x1F513; Unlock</button>
    <button class='btn' onclick='C("lock")'>&#x1F512; Lock</button>
  </div>
  <hr class='divider'>
  <label>Max Current (A)</label>
  <div style='display:flex;gap:10px;align-items:center'>
    <input type='range' id='sl' min='6' max='32' value='32'
      oninput='document.getElementById("sv").textContent=this.value+"A"'
      onchange='setCurrent(this.value)'
      style='flex:1'>
    <span id='sv' style='font-weight:600;min-width:36px'>32A</span>
  </div>
</div>

<script>
var SN={0:'Disconnected',1:'Connected',2:'Charging',3:'Paused',4:'Scheduled',5:'Discharging',6:'Error',7:'Disconnected',8:'Locked',9:'Updating',10:'Queue (Power)',13:'Waiting for Car',14:'Error',16:'Ready',17:'Connected',18:'Waiting for Schedule',19:'Scheduled',20:'Charging',21:'Charge Complete',22:'Paused by User',23:'Queue (Power Share)',24:'Queue (Eco Smart)',25:'Waiting for Schedule',26:'Discharging',161:'Ready',178:'Paused',179:'Charging',180:'Scheduled',189:'Ready',193:'Paused',194:'Locked',209:'Reserved (OCPP)',210:'Updating'};
function P(){fetch('/api/charger').then(function(r){return r.json()}).then(function(d){if(!d.status||d.status==='null')return;var s=d.status?d.status.r:null,rt=d.realtime?d.realtime.r:null;if(s){var n=SN[s.st];if(!n&&rt)n=SN[rt.charger_status];document.getElementById('v-st').textContent=n||'Code '+s.st;document.getElementById('v-pw').textContent=(+s.cp).toFixed(2)+' kW';var threePhase=(s.L2>0||s.L3>0||(rt&&rt.phases_connection>=2));var l1=(s.L1/10).toFixed(1),l2=(s.L2/10).toFixed(1),l3=(s.L3/10).toFixed(1);if(threePhase){document.getElementById('l-cr').textContent='L1 / L2 / L3';document.getElementById('v-cr').textContent=l1+' / '+l2+' / '+l3+' A'}else{document.getElementById('l-cr').textContent='Charging Current';document.getElementById('v-cr').textContent=l1+' A'}document.getElementById('v-en').textContent=(s.en/100).toFixed(2)+' kWh';document.getElementById('v-mc').textContent=s.cur+' A';document.getElementById('sl').value=s.cur;document.getElementById('sv').textContent=s.cur+'A'}if(rt){document.getElementById('v-lk').textContent=rt.lock_status==0?'Unlocked':'Locked';var os={0:'Not Available',1:'Not Configured',2:'Connected',3:'Charging'};var oe=document.getElementById('v-oc');if(oe)oe.textContent=os[rt.ocpp_status]||'Code '+rt.ocpp_status}}).catch(function(){});fetch('/api/command?action=bapi&met=r_dca&par=null').then(function(r){return r.json()}).then(function(d){if(d.r){document.getElementById('v-vt').textContent=d.r.v1+' V';document.getElementById('v-gp').textContent=d.r.p1+' W'}}).catch(function(){})}
function C(a){toast('Sending '+a.split('&')[0]+'...','info');fetch('/api/command?action='+a).then(function(x){return x.json()}).then(function(d){if(d.error)toast(d.error,'error');else toast('Command sent','success');setTimeout(P,1000)}).catch(function(e){toast('Error: '+e,'error')})}
var _curTimer=null;
function setCurrent(v){if(_curTimer)clearTimeout(_curTimer);_curTimer=setTimeout(function(){toast('Setting '+v+'A','info');fetch('/api/command?action=current&value='+v).then(function(x){return x.json()}).then(function(d){if(d.error)toast(d.error,'error');else toast('Max current set to '+v+'A','success');setTimeout(P,1000)}).catch(function(e){toast('Error: '+e,'error')})},300)}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
P();setInterval(P,10000);
fetch('/api/command?action=bapi&met=read_pin&par=null',{signal:AbortSignal.timeout(8000)}).then(function(r){return r.json()}).then(function(d){if(d.r&&!d.r.pin)document.getElementById('pin-warn').style.display='block'}).catch(function(){});
</script>
</div>
)HTML";
    page += htmlFoot("/");
    http.send(200, "text/html", page);
}

// ========== PAGE 2: Settings (/settings) ==========
static void handleSettings() {
    String page = htmlHead("Settings");
    page += R"HTML(
<div class='loading' id='ld'><div class='ld-spin'></div>Loading Settings...</div>
<div id='pg' style='display:none'>
<h1>&#x2699; Settings</h1>

<style>
.tab-wrap{position:relative}
.tab-wrap::after{content:'';position:absolute;top:0;right:0;bottom:2px;width:20px;background:linear-gradient(to right,transparent,var(--bg));pointer-events:none}
.tabs{display:flex;gap:0;border-bottom:2px solid var(--border);margin-bottom:14px;overflow-x:auto;-webkit-overflow-scrolling:touch;scrollbar-width:none}
.tabs::-webkit-scrollbar{display:none}
.tab{padding:10px 16px;color:var(--text3);font-size:.85em;font-weight:600;cursor:pointer;border-bottom:2px solid transparent;margin-bottom:-2px;white-space:nowrap;transition:all .2s;user-select:none}
.tab.active{color:var(--primary);border-bottom-color:var(--primary)}
.tab:hover{color:var(--text)}
.tab-panel{display:none;min-height:40vh}.tab-panel.active{display:block}
#qr{transition:all .2s ease;overflow:hidden}
</style>

<div class='tab-wrap'><div class='tabs'>
  <div class='tab active' onclick='showTab(0)'>Schedules</div>
  <div class='tab' onclick='showTab(1)'>Power</div>
  <div class='tab' onclick='showTab(2)'>Security</div>
  <div class='tab' onclick='showTab(3)'>Charger</div>
</div></div>

<!-- TAB 0: Schedules -->
<div class='tab-panel active' id='tp0'>
  <div class='card'>
    <button class='btn btn-outline' style='margin-bottom:12px' onclick='Q("r_schs","Schedules")'>View Current Schedules</button>
    <button class='btn btn-outline' onclick='loadSessions()'>View Session History</button>
    <a href='/sessions' class='btn btn-outline' style='display:block;text-decoration:none;text-align:center;margin-top:8px'>&#x1F4CA; Weekly Heatmap</a>
    <div id='qr' style='display:none;margin-top:12px'></div>
  </div>

  <div class='card' id='sched-edit' style='display:none'>
  <div class='card-header'><span class='card-icon'>&#x1F4C5;</span><h2>Edit Schedule</h2></div>
  <div class='row'>
    <div><label>Start</label><input type='time' id='ss' value='14:00'></div>
    <div><label>Stop</label><input type='time' id='se' value='20:00'></div>
  </div>
  <label>Days</label>
  <div id='sd' style='display:flex;flex-wrap:wrap;gap:4px;margin:4px 0 12px'>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='1'>Mon</label>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='2'>Tue</label>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='4'>Wed</label>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='8'>Thu</label>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='16'>Fri</label>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='32'>Sat</label>
    <label style='display:flex;align-items:center;gap:3px;background:var(--elevated);padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.85em'><input type='checkbox' value='64'>Sun</label>
  </div>
  <div class='row'>
    <div><label>Power Limit (A)</label><input type='number' id='sc' min='6' max='32' value='32' placeholder='32 = no limit'></div>
    <div><label>Energy Limit (kWh)</label><input type='number' id='se2' min='0' max='100' value='0' placeholder='0 = no limit'></div>
  </div>
  <div><label>Enabled</label><select id='sn'><option value='1'>Yes</option><option value='0'>No</option></select></div>
  <div class='row' style='margin-top:10px'>
    <button class='btn btn-success' onclick='saveSch()'>Save Schedule</button>
    <button class='btn btn-outline' onclick='delSch()'>Delete All</button>
  </div>
  <div id='sr' style='display:none;margin-top:10px'></div>
  </div>
</div>

<!-- TAB 1: Power -->
<div class='tab-panel' id='tp1'>
  <div class='card'>
    <div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("r_dca","Energy Meter")'>&#x1F50B; Energy Meter</button>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("g_ecos","Eco Smart")'>&#x2600; Eco Smart</button>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("r_hsh","Power Boost")'>&#x26A1; Power Boost</button>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("g_psh","Power Sharing")'>&#x1F50C; Power Sharing</button>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("g_phsw","Phase Switch")'>&#x1F504; Phase Switch</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("eco")'>&#x2600; Set Eco Smart</button>
    </div>
    <div id='qr1' style='display:none;margin-top:12px'></div>
  </div>
</div>

<!-- TAB 2: Security -->
<div class='tab-panel' id='tp2'>
  <div class='card'>
    <div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("g_alo","Auto Lock")'>&#x1F512; Auto Lock</button>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("read_pin","BLE PIN")'>&#x1F511; BLE PIN</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("autolock")'>&#x1F510; Set Auto Lock</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("ocpp")'>&#x1F517; OCPP Setup</button>
    </div>
    <div id='qr2' style='display:none;margin-top:12px'></div>
  </div>
</div>

<!-- TAB 3: Charger -->
<div class='tab-panel' id='tp3'>
  <div class='card'>
    <div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>
      <button class='btn btn-outline' style='padding:12px' onclick='E("tz")'>&#x1F30D; Timezone</button>
      <button class='btn btn-outline' style='padding:12px' onclick='Q("gwsta","WiFi Status")'>&#x1F4F6; WiFi Status</button>
      <button class='btn btn-outline' style='padding:12px' onclick='confirm2("Reboot the charger?",function(){fetch("/api/command?action=reboot").then(function(){toast("Reboot sent","success")})})' style='background:rgba(239,68,68,.08);border-color:var(--danger);color:var(--danger)'>&#x1F504; Reboot</button>
    </div>
    <div id='qr3' style='display:none;margin-top:12px'></div>
  </div>
</div>

<script>
function showTab(n){document.querySelectorAll('.tab-panel').forEach(function(p,i){p.classList.toggle('active',i===n)});document.querySelectorAll('.tab').forEach(function(t,i){t.classList.toggle('active',i===n)});document.getElementById('qr').style.display='none';document.getElementById('qr').innerHTML=''}
var DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var CHARGER_TZ='UTC';
try{CHARGER_TZ=Intl.DateTimeFormat().resolvedOptions().timeZone||'UTC'}catch(e){}
var tzReady=fetch('/api/command?action=bapi&met=g_tzn&par=null',{signal:AbortSignal.timeout(8000)}).then(function(r){return r.json()}).then(function(d){if(d.r&&d.r.timezone)CHARGER_TZ=d.r.timezone}).catch(function(){});
function utcToLocal(hhmm){var h=parseInt(hhmm.slice(0,2)),m=parseInt(hhmm.slice(2));try{return new Date(Date.UTC(2024,0,1,h,m)).toLocaleTimeString('en-AU',{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){var d=new Date();d.setUTCHours(h,m,0,0);return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)}}
function localToUtc(hhmm){var p=hhmm.split(':');try{var fmt=new Intl.DateTimeFormat('en-US',{timeZone:CHARGER_TZ,hour:'numeric',minute:'numeric',hour12:false});var d=new Date(2024,0,1,parseInt(p[0]),parseInt(p[1]));var utc=new Date(d.toLocaleString('en-US',{timeZone:'UTC'}));var local=new Date(d.toLocaleString('en-US',{timeZone:CHARGER_TZ}));var diff=local-utc;var ud=new Date(d.getTime()-diff);return ('0'+ud.getUTCHours()).slice(-2)+('0'+ud.getUTCMinutes()).slice(-2)}catch(e){var d2=new Date();d2.setHours(parseInt(p[0]),parseInt(p[1]),0,0);return ('0'+d2.getUTCHours()).slice(-2)+('0'+d2.getUTCMinutes()).slice(-2)}}
function Q(m,l){var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;if(!r)r=document.getElementById('qr');if(!r){toast('No result panel found','error');return}r.style.display='block';r.innerHTML="<span class='spinner'></span>"+l+"...";var doFetch=function(){fetch('/api/command?action=bapi&met='+m+'&par=null',{signal:AbortSignal.timeout(15000)}).then(function(x){return x.json()}).then(function(d){if(d.error){r.innerHTML='<span style="color:var(--danger)">'+d.error+'</span>';return}r.innerHTML=F(m,l,d.r||d)}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'})};if(m==='r_schs'&&tzReady){tzReady.then(doFetch)}else{doFetch()}}
function F(m,l,r){var h="<div style='font-weight:600;color:var(--accent);margin-bottom:6px'>"+l+"</div>";if(m==='gwsta'){var s={0:'Disconnected',1:'Connected',2:'Connecting'};h+=row('WiFi',s[r]||'Code '+r)}else if(m==='r_ses'){h+=row('Last Session',r.last);h+=row('Total Sessions',r.size)}else if(m==='r_schs'){var sc=r.schedules||r;if(!Array.isArray(sc)){h+=row('Schedules','None');return h}sc.forEach(function(s,i){var ds='';for(var b=0;b<7;b++)if(s.days&(1<<b))ds+=DAYS[b]+' ';h+="<div style='background:var(--bg);border-radius:8px;padding:8px;margin:4px 0'><div style='display:flex;justify-content:space-between'><b>Schedule "+(i+1)+"</b><span class='badge "+(s.enabled?'badge-success':'badge-warning')+"'>"+(s.enabled?'Active':'Off')+"</span></div>";h+=row('Time',utcToLocal(s.start)+' - '+utcToLocal(s.stop));h+=row('Days',ds.trim()||'None');h+=row('Power Limit',(s.mcr<=1||s.mcr>=32)?'No limit':s.mcr+' A');if(s.target&&s.target.type==1)h+=row('Energy Limit',(s.target.value/1000)+' kWh');else h+=row('Energy Limit','No limit');h+="</div>"});if(!sc.length)h+=row('Schedules','None')}else if(m==='r_dca'){if(typeof r==='object'){h+=row('Voltage L1',r.v1+' V');if(r.v2)h+=row('Voltage L2',r.v2+' V');if(r.v3)h+=row('Voltage L3',r.v3+' V');h+=row('Power L1',r.p1+' W');if(r.p2)h+=row('Power L2',r.p2+' W');if(r.p3)h+=row('Power L3',r.p3+' W');h+=row('Current L1',(r.c1/10).toFixed(1)+' A');if(r.c2)h+=row('Current L2',(r.c2/10).toFixed(1)+' A');if(r.c3)h+=row('Current L3',(r.c3/10).toFixed(1)+' A');h+=row('Total Energy',(r.e/1000).toFixed(1)+' kWh')}else{h+=row('Energy Meter',''+r)}}else if(m==='g_alo'){if(typeof r==='object'){h+=row('Auto Lock',r.enabled?'Enabled':'Disabled');if(r.time)h+=row('Lock After',r.time+' seconds')}else{h+=row('Auto Lock',r?'Enabled':'Disabled')}}else if(m==='g_ecos'){var em={0:'Disabled',1:'Eco Smart (Solar + Grid)',2:'Full Green (Solar Only)'};if(typeof r==='object'){h+=row('Status',r.ese?'Active':'Inactive');h+=row('Mode',em[r.esm]||'Mode '+r.esm);h+=row('Solar Power Target',r.esp+'%')}else{h+=row('Eco Smart',em[r]||''+r)}}else if(m==='g_phsw'){if(typeof r==='object'){h+=row('Phase Switch',r.enabled?'Enabled':'Disabled')}else{h+=row('Phase Switch',r?'Enabled':'Disabled')}}else if(m==='read_pin'){if(typeof r==='object'){h+=row('BLE PIN',r.pin||'Not set');h+=row('PIN Version',r.version||'None')}else{h+=row('BLE PIN',''+r)}}else if(m==='r_hsh'){h+=row('ICP Max Current',r+'A')}else if(m==='g_psh'){if(typeof r==='object'){h+=row('Dynamic Power Sharing',r.dyps?'Enabled':'Disabled');h+=row('Max Power Per Charger',r.mcpp?r.mcpp+'W':'Unlimited');h+=row('Min Current',r.minI+'A');h+=row('Chargers in Group',r.nchg)}else{var ps={0:'Disabled',1:'Enabled',2:'Active'};h+=row('Power Sharing',ps[r]||''+r)}}else if(m==='g_tzn'){h+=row('Timezone',r.timezone||r)}else{h+="<pre style='margin:0;white-space:pre-wrap;font-size:.82em'>"+JSON.stringify(r,null,2)+"</pre>"}return h}
function saveSch(){var st=localToUtc(document.getElementById('ss').value),sp=localToUtc(document.getElementById('se').value),d=0;document.querySelectorAll('#sd input:checked').forEach(function(c){d+=parseInt(c.value)});if(!d){toast('Select at least one day','error');return}var mcr=parseInt(document.getElementById('sc').value);var ekwh=parseInt(document.getElementById('se2').value)||0;var tgt=ekwh>0?{type:1,value:ekwh*1000}:{type:0,value:0};var p=JSON.stringify({sid:0,start:st,stop:sp,days:d,enabled:parseInt(document.getElementById('sn').value),mcr:mcr,repeat:1,type:0,name:'',target:tgt});toast('Saving schedule...','info');fetch('/api/command?action=bapi&met=w_sch&par='+encodeURIComponent(p)).then(function(x){return x.json()}).then(function(d){toast(d.error||'Schedule saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e,'error')})}
function showTimezone(){E('tz')}
function saveTz(){var tz=document.getElementById('tz-select').value;var p=JSON.stringify({timezone:tz});toast('Saving timezone...','info');fetch('/api/command?action=bapi&met=s_tzn&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){if(d.error){toast(d.error,'error')}else{CHARGER_TZ=tz;toast('Timezone set to '+tz,'success')}}).catch(function(e){toast('Error: '+e.message,'error')})}
function E(type){var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;if(!r)return;r.style.display='block';var h='';if(type==='autolock'){h="<h2>&#x1F510; Auto Lock</h2><label>Enabled</label><select id='al-en'><option value='0'>Disabled</option><option value='1'>Enabled</option></select><label>Lock After (seconds)</label><input type='number' id='al-time' value='60' min='10' max='600'><div class='row' style='margin-top:10px'><button class='btn btn-success' onclick='saveAutoLock()'>Save</button></div><div id='al-result' style='display:none;margin-top:10px'></div>"}else if(type==='ocpp'){h="<h2>&#x1F517; OCPP</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_ocpp&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){var o=d.r||{};r.innerHTML="<h2>&#x1F517; OCPP Configuration</h2><label>Server URL</label><input id='ocpp-url' value='"+(o.u||'')+"' placeholder='ws://server:9000'><div class='row'><div><label>Charger ID</label><input id='ocpp-id' value='"+(o.chid||'')+"'></div><div><label>Password</label><input id='ocpp-pw' type='password' value='"+(o.pw||'')+"'></div></div><label>Enabled</label><select id='ocpp-en'><option value='0'"+(o.e?'':' selected')+">Disabled</option><option value='1'"+(o.e?' selected':'')+">Enabled</option></select><button class='btn btn-success' style='margin-top:10px' onclick='saveOcpp()'>Save OCPP</button><div id='ocpp-result' style='display:none;margin-top:10px'></div>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='eco'){h="<h2>&#x2600; Eco Smart</h2><label>Mode</label><select id='eco-mode'><option value='0'>Disabled</option><option value='1'>Eco Smart (Solar + Grid)</option><option value='2'>Full Green (Solar Only)</option></select><p class='help'>Eco Smart uses solar surplus. Full Green only charges from solar.</p><button class='btn btn-success' style='margin-top:10px' onclick='saveEco()'>Save</button><div id='eco-result' style='display:none;margin-top:10px'></div>"}else if(type==='tz'){h="<h2>&#x1F30D; Timezone</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_tzn&par=null',{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){var tz=d.r?d.r.timezone:'Unknown';CHARGER_TZ=tz;var opts='';var zones=['Australia/Sydney','Australia/Melbourne','Australia/Brisbane','Australia/Adelaide','Australia/Perth','Australia/Darwin','Australia/Hobart','Asia/Tokyo','Asia/Shanghai','Asia/Singapore','Asia/Kolkata','Asia/Dubai','Europe/London','Europe/Paris','Europe/Berlin','America/New_York','America/Chicago','America/Los_Angeles','America/Toronto','Pacific/Auckland','UTC'];zones.forEach(function(z){opts+="<option value='"+z+"'"+(z===tz?' selected':'')+">"+z.replace('_',' ')+"</option>"});r.innerHTML="<h2>&#x1F30D; Timezone</h2>"+row('Current',tz)+"<label style='margin-top:12px'>Change To</label><select id='tz-select'>"+opts+"</select><button class='btn btn-success' style='margin-top:10px' onclick='saveTz()'>Save</button><div id='tz-result' style='display:none;margin-top:10px'></div>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}r.innerHTML=h}
function showAutoLock(){E('autolock')}
function saveAutoLock(){var p=JSON.stringify({enabled:parseInt(document.getElementById('al-en').value),time:parseInt(document.getElementById('al-time').value)});toast('Saving auto lock...','info');fetch('/api/command?action=bapi&met=s_alo&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'Auto lock saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function showEcoSmart(){E('eco')}
function saveEco(){var mode=parseInt(document.getElementById('eco-mode').value);var p=JSON.stringify({mode:mode});toast('Saving eco smart...','info');fetch('/api/command?action=bapi&met=s_ecos&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'Eco Smart saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function loadOcpp(){E('ocpp')}
function saveOcpp(){var p=JSON.stringify({u:document.getElementById('ocpp-url').value,chid:document.getElementById('ocpp-id').value,pw:document.getElementById('ocpp-pw').value,e:parseInt(document.getElementById('ocpp-en').value)});toast('Saving OCPP...','info');fetch('/api/command?action=bapi&met=s_ocpp&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'OCPP config saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function loadSessions(){
  var r=document.getElementById('qr');r.style.display='block';
  r.innerHTML="<span class='spinner'></span>Loading sessions...";
  fetch('/api/command?action=bapi&met=r_ses&par=null',{signal:AbortSignal.timeout(15000)})
    .then(function(x){return x.json()}).then(function(d){
      if(!d.r||!d.r.last){r.innerHTML='<span style="color:var(--text3)">No sessions yet</span>';return}
      var last=d.r.last,count=Math.min(20,last);
      var h="<div style='font-weight:600;color:var(--accent);margin-bottom:8px'>Last "+count+" Sessions (total: "+last+")</div>";
      h+="<div id='ses-list'><span class='spinner'></span> Fetching details...</div>";
      r.innerHTML=h;
      // Fetch each session detail sequentially (BLE can't handle parallel)
      var items=[],idx=0,start=last-count+1;
      function next(){
        if(idx>=count){
          var html='';
          items.forEach(function(s){
            var dur=s.dur?Math.round(s.dur/60)+'m':'-';
            var en=s.en?(s.en/100).toFixed(2)+' kWh':'-';
            var ts=s.ts?new Date(s.ts*1000).toLocaleString():'Session #'+s.id;
            html+="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:4px 0;display:flex;justify-content:space-between'><div><div style='font-size:.82em;color:var(--text2)'>"+ts+"</div><div style='font-size:.78em;color:var(--text3)'>Session #"+s.id+"</div></div><div style='text-align:right'><div style='font-weight:600'>"+en+"</div><div style='font-size:.78em;color:var(--text3)'>"+dur+"</div></div></div>";
          });
          document.getElementById('ses-list').innerHTML=html||'<span style="color:var(--text3)">No details</span>';
          return;
        }
        var sid=start+idx;
        fetch('/api/command?action=bapi&met=r_log&par='+sid,{signal:AbortSignal.timeout(10000)})
          .then(function(x){return x.json()}).then(function(sd){
            if(sd.r){items.push({id:sid,dur:sd.r.dur||sd.r.duration,en:sd.r.en||sd.r.energy,ts:sd.r.ts||sd.r.timestamp||sd.r.start})}
            idx++;next();
          }).catch(function(){idx++;next()});
      }
      next();
    }).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'});
}
function delSch(){confirm2('Delete all schedules?',delSch2)}
function delSch2(){var r=document.getElementById('sr');r.style.display='block';r.innerHTML="<span class='spinner'></span>Deleting...";fetch('/api/command?action=bapi&met=clr_sch&par=null').then(function(x){return x.json()}).then(function(d){r.innerHTML=d.error?'<span style="color:var(--danger)">'+d.error+'</span>':'<span style="color:var(--success)">Deleted</span>'}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e+'</span>'})}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
</script>
</div>
)HTML";
    page += htmlFoot("/settings");
    http.send(200, "text/html", page);
}

// ========== PAGE 3: Config (/config) ==========
static void handleConfig() {
    const WBConfig& cfg = configMgr.get();
    String page = htmlHead("Config");

    page += "<h1>&#x1F527; Configuration</h1>";

    bool wifiOk = WiFi.status() == WL_CONNECTED;
    bool bleOk = wallboxBLE.isConnected();
    page += "<div class='status-bar'>";
    page += "<span class='status-item'><span class='status-dot " + String(wifiOk ? "dot-green" : "dot-red") + "'></span>" + (wifiOk ? WiFi.localIP().toString() : String("No WiFi")) + "</span>";
    page += "<span class='status-item'><span class='status-dot " + String(bleOk ? "dot-green" : "dot-yellow") + "'></span>BLE: " + String(wallboxBLE.stateStr()) + "</span>";
    page += "</div>";

    ensureCsrfToken();
    page += "<form method='POST' action='/save'>";
    page += "<input type='hidden' name='csrf' value='" + csrfToken + "'>";

    // WiFi
    page += "<div class='card'><div class='card-header'><span class='card-icon'>&#x1F4F6;</span><h2>WiFi</h2></div>";
    page += "<label>SSID</label><div style='display:flex;gap:8px;align-items:stretch'>";
    page += "<input id='wifi_ssid' name='wifi_ssid' value='" + cfg.wifiSSID + "' placeholder='Type SSID or scan' style='flex:1;margin:0'>";
    page += "<button type='button' id='wifiScanBtn' class='btn btn-outline' style='width:80px;padding:0;margin:0' onclick='scanWifi()'>Scan</button></div>";
    page += "<div id='wifi-results' style='display:none;background:var(--elevated);border-radius:8px;padding:6px;margin-top:-6px;margin-bottom:14px;max-height:200px;overflow-y:auto'></div>";
    page += "<label>Password</label><input type='password' name='wifi_pass' value='" + cfg.wifiPass + "'></div>";

    // MQTT
    page += "<div class='card'><div class='card-header'><span class='card-icon'>&#x1F3E0;</span><h2>MQTT</h2></div>";
    page += "<label>Host</label><input name='mqtt_host' value='" + cfg.mqttHost + "' placeholder='homeassistant.local'>";
    page += "<div class='row'><div><label>Port</label><input name='mqtt_port' type='number' value='" + String(cfg.mqttPort) + "'></div>";
    page += "<div><label>Client ID</label><input name='mqtt_cid' value='" + cfg.mqttClientId + "'></div></div>";
    page += "<label>Username</label><input name='mqtt_user' value='" + cfg.mqttUser + "' placeholder='optional'>";
    page += "<label>Password</label><input type='password' name='mqtt_pass' value='" + cfg.mqttPass + "' placeholder='optional'></div>";

    // BLE
    page += "<div class='card'><div class='card-header'><span class='card-icon'>&#x1F50B;</span><h2>Charger BLE</h2></div>";
    page += "<button type='button' id='scanBtn' class='btn btn-outline' style='margin-bottom:12px' onclick='startScan()'>Scan for Chargers</button>";
    page += "<div id='scanResults'></div>";
    page += "<label>BLE Address</label><input id='ble_addr' name='ble_addr' value='" + cfg.bleAddr + "' placeholder='6C1DEB309808' oninput='formatMAC(this)'>";
    page += "<p class='help'>With or without colons</p>";
    page += "<label>BLE PIN</label><input name='ble_pin' value='" + cfg.blePin + "' placeholder='Empty = no PIN'></div>";

    // Security
    page += "<div class='card'><div class='card-header'><span class='card-icon'>&#x1F512;</span><h2>Web Security</h2></div>";
    page += "<label>Enable Authentication</label>";
    page += "<select name='auth_en'><option value='0'" + String(cfg.authEnabled ? "" : " selected") + ">Disabled</option><option value='1'" + String(cfg.authEnabled ? " selected" : "") + ">Enabled</option></select>";
    page += "<div class='row'>";
    page += "<div><label>Username</label><input name='auth_user' value='" + cfg.authUser + "'></div>";
    page += "<div><label>Password</label><input type='password' name='auth_pass' value='" + cfg.authPass + "'></div>";
    page += "</div>";
    page += "<p class='help'>When enabled, all control actions and OTA require login. Dashboard viewing remains open.</p></div>";

    // Advanced
    page += "<details><summary style='color:var(--text3);cursor:pointer;padding:6px 0;font-size:.85em'>Advanced</summary><div class='card'>";
    page += "<label>Service UUID</label><input name='ble_svc' value='" + cfg.bleService + "' style='font-size:12px;font-family:monospace'>";
    page += "<label>Char UUID</label><input name='ble_chr' value='" + cfg.bleChar + "' style='font-size:12px;font-family:monospace'>";
    page += "<div class='row'><div><label>Status Poll (ms)</label><input name='poll_status' type='number' value='" + String(cfg.statusPollMs) + "'></div>";
    page += "<div><label>Realtime Poll (ms)</label><input name='poll_rt' type='number' value='" + String(cfg.realtimePollMs) + "'></div></div>";
    page += "<div class='row'><div><label>HA Prefix</label><input name='ha_prefix' value='" + cfg.haDiscoveryPrefix + "'></div>";
    page += "<div><label>Device ID</label><input name='ha_devid' value='" + cfg.haDeviceId + "'></div></div></div></details>";

    page += "<button type='submit' class='btn btn-success' style='margin-top:12px'>&#x1F4BE; Save &amp; Reboot</button></form>";
    page += "<a href='/ota' class='btn btn-outline' style='margin-top:10px'>&#x1F4E6; Firmware Update</a>";
    page += "<button type='button' class='btn btn-danger' style='margin-top:10px' onclick='confirm2(\"Erase all settings and reboot into setup mode?\",function(){var f=document.createElement(\"form\");f.method=\"POST\";f.action=\"/reset\";var i=document.createElement(\"input\");i.type=\"hidden\";i.name=\"csrf\";i.value=\"" + csrfToken + "\";f.appendChild(i);document.body.appendChild(f);f.submit()})'>&#x1F5D1; Factory Reset</button>";

    page += htmlFoot("/config");
    http.send(200, "text/html", page);
}

// ========== PAGE 4: Info (/info) ==========
static void handleInfo() {
    String page = htmlHead("Info");
    page += R"HTML(
<div class='loading' id='ld'><div class='ld-spin'></div>Loading Info...</div>
<div id='pg' style='display:none'>
<h1>&#x2139; Gateway Info</h1>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4E1;</span><h2>Gateway</h2></div>
  <div id='gw'>Loading...</div>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F50C;</span><h2>Charger Details</h2></div>
  <div id='chg'>Loading...</div>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F517;</span><h2>Charger Info</h2></div>
  <div style='display:grid;grid-template-columns:1fr 1fr;gap:8px'>
    <button class='btn btn-outline' style='padding:12px' onclick='Q("g_ocpp","OCPP")'>OCPP Config</button>
    <button class='btn btn-outline' style='padding:12px' onclick='Q("gnsta","Network")'>Network Status</button>
    <button class='btn btn-outline' style='padding:12px' onclick='Q("r_not","Notifications")'>Notifications</button>
    <button class='btn btn-outline' style='padding:12px' onclick='Q("gwsta","WiFi")'>WiFi Status</button>
  </div>
  <div id='qr' style='margin-top:12px;display:none'></div>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4E1;</span><h2>Raw BAPI</h2></div>
  <div style='display:grid;grid-template-columns:1fr auto;gap:8px;align-items:end'>
    <div><label>Method</label><select id='bm' style='margin:0'>
      <optgroup label='Status'>
        <option value='r_dat'>r_dat - Full Status</option>
        <option value='r_sta'>r_sta - Realtime Status</option>
        <option value='ping'>ping - Ping</option>
      </optgroup>
      <optgroup label='Energy'>
        <option value='r_ses'>r_ses - Session Info</option>
        <option value='r_log'>r_log - Session Log</option>
        <option value='r_hsh'>r_hsh - Power Boost (ICP)</option>
        <option value='g_psh'>g_psh - Power Sharing</option>
        <option value='r_dca'>r_dca - Power Meter</option>
      </optgroup>
      <optgroup label='Config'>
        <option value='gulck'>gulck - Lock Status</option>
        <option value='g_tzn'>g_tzn - Timezone</option>
        <option value='r_schs'>r_schs - Schedules</option>
        <option value='g_ocpp'>g_ocpp - OCPP Config</option>
        <option value='g_phsw'>g_phsw - Phase Switch</option>
        <option value='read_pin'>read_pin - BLE PIN</option>
      </optgroup>
      <optgroup label='Network'>
        <option value='gnsta'>gnsta - Network Status</option>
        <option value='gwsta'>gwsta - WiFi Status</option>
        <option value='gwnet'>gwnet - WiFi Networks</option>
        <option value='gmsta'>gmsta - GSM Status</option>
        <option value='gmcon'>gmcon - Mobile Config</option>
      </optgroup>
      <optgroup label='System'>
        <option value='r_not'>r_not - Notifications</option>
        <option value='r_wel'>r_wel - Grounding Status</option>
        <option value='gpmod'>gpmod - Proxy Mode</option>
      </optgroup>
    </select></div>
    <button class='btn' style='height:44px;width:72px;margin:0' onclick='B()'>Send</button>
  </div>
  <p class='help' style='margin-top:4px'>Select a command and press Send to query the charger directly</p>
  <pre id='br' style='background:var(--bg);border-radius:8px;padding:10px;margin-top:10px;white-space:pre-wrap;max-height:250px;overflow:auto;display:none;font-size:.82em'></pre>
</div>

<div class='card'>
  <a href='/ota' class='btn btn-outline' style='text-decoration:none;display:block'>&#x1F4E6; Firmware Update</a>
</div>
<p style='text-align:center;color:var(--text3);font-size:.75em;margin-top:16px'>Wallbox BLE Gateway v1.0</p>

<script>
function loadGW(){fetch('/api/status').then(function(r){return r.json()}).then(function(d){var h='';h+=row('WiFi',d.ssid+' ('+d.ip+')');h+=row('WiFi Signal',d.wifi_rssi+' dBm');h+=row('BLE State',d.ble);h+=row('BLE Signal',d.rssi+' dBm');h+=row('Commands Sent',d.tx);h+=row('Responses',d.rx);var m=Math.floor(d.uptime/60),hr=Math.floor(m/60);h+=row('Uptime',hr+'h '+m%60+'m');h+=row('Free Memory',Math.round(d.heap/1024)+' KB');document.getElementById('gw').innerHTML=h;var c='';if(d.dev_name)c+=row('Name',d.dev_name);if(d.dev_mfg)c+=row('Manufacturer',d.dev_mfg);if(d.dev_model)c+=row('Model',d.dev_model);if(d.dev_fw)c+=row('BLE Module FW',d.dev_fw);document.getElementById('chg').innerHTML=c||'<span style="color:var(--text3)">Connect BLE to see charger details</span>'})}
var OCPP_LABELS={chid:'Charger ID',e:'Enabled',pw:'Password',u:'Server URL',ws:'WebSocket',id:'Identity',status:'Status',connected:'Connected',protocol:'Protocol',interval:'Heartbeat (s)',auth:'Auth Type'};
var NET_LABELS={channel:'WiFi Channel',dns1:'DNS Primary',dns2:'DNS Secondary',gateway:'Gateway',ip:'IP Address',netmask:'Subnet Mask',mac:'MAC Address',ssid:'Network Name',rssi:'Signal (dBm)',signal:'Signal',status:'Status',type:'Connection Type'};
function Q(m,l){var r=document.getElementById('qr');r.style.display='block';r.innerHTML="<span class='spinner'></span>"+l+"...";fetch('/api/command?action=bapi&met='+m+'&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){if(d.error){r.innerHTML='<span style="color:var(--danger)">'+d.error+'</span>';return}var v=d.r||d;var h="<div style='font-weight:600;color:var(--accent);margin-bottom:6px'>"+l+"</div>";if(m==='gwsta'){var ws={0:'Disconnected',1:'Connected',2:'Connecting'};h+=row('WiFi Status',typeof v==='number'?(ws[v]||'Code '+v):''+v)}else if(m==='r_not'){if(Array.isArray(v)){if(v.length===0)h+=row('Notifications','None');else v.forEach(function(n,i){h+="<div style='background:var(--bg);border-radius:8px;padding:8px;margin:4px 0'>";h+=row('#'+(i+1),n.message||n.msg||JSON.stringify(n));h+="</div>"})}else{h+=row('Notifications',typeof v==='number'?(v===0?'None':''+v):''+v)}}else if(m==='g_ocpp'){var lb=OCPP_LABELS;if(typeof v==='object'){for(var k in v){var lbl=lb[k]||k;var val=v[k];if(val===null||val===undefined||val==='')val='<span style="color:var(--text3)">Not set</span>';else if(typeof val==='number'&&(k==='e'||k==='connected'))val=val?'Yes':'No';h+=row(lbl,val)}}else{h+=row('OCPP',v===1?'Connected':v===0?'Not configured':'Code '+v)}}else if(m==='gnsta'){if(Array.isArray(v)&&v.length>0){var n=v[0];var lb=NET_LABELS;for(var k in n){var lbl=lb[k]||k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});var val=n[k];if(val===''||val===null)val='<span style="color:var(--text3)">-</span>';h+=row(lbl,val)}}else if(typeof v==='object'&&!Array.isArray(v)){var lb=NET_LABELS;for(var k in v){var lbl=lb[k]||k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});var val=v[k];if(val===''||val===null)val='<span style="color:var(--text3)">-</span>';h+=row(lbl,val)}}else{h+=row('Network',''+v)}}else if(m==='gupdc'){if(typeof v==='object'){if(v.update)h+=row('Update Available','<span style="color:var(--success)">Yes</span>');else h+=row('Update Available','No');if(v.version||v.current)h+=row('Current Version',v.version||v.current||'Unknown');if(v.latest||v.new_version)h+=row('Latest Version',v.latest||v.new_version||'Unknown')}else{h+=row('Firmware',typeof v==='number'?(v===0?'Up to date':'Update available ('+v+')'):''+v)}}else if(m==='r_not'){if(typeof v==='object'){if(Array.isArray(v)){if(v.length===0)h+=row('Notifications','None');v.forEach(function(n,i){h+="<div style='background:var(--bg);border-radius:8px;padding:8px;margin:4px 0'>";h+=row('Notification '+(i+1),n.message||n.msg||n.text||JSON.stringify(n));if(n.timestamp||n.ts)h+=row('Time',new Date((n.timestamp||n.ts)*1000).toLocaleString());h+="</div>"})}else{for(var k in v){var lbl=k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});h+=row(lbl,typeof v[k]==='object'?JSON.stringify(v[k]):v[k])}}}else{h+=row('Notifications',v===0?'None':''+v)}}else if(typeof v==='object'){for(var k in v){var lbl=k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});var val=v[k];if(typeof val==='boolean')val=val?'Yes':'No';else if(typeof val==='object')val=JSON.stringify(val);h+=row(lbl,val)}}else{h+=row(l,v)}r.innerHTML=h}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'})}
function B(){var m=document.getElementById('bm').value,r=document.getElementById('br');r.style.display='block';r.textContent='Sending '+m+'...';fetch('/api/command?action=bapi&met='+encodeURIComponent(m)+'&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){r.textContent=JSON.stringify(d,null,2)}).catch(function(e){r.textContent='Error: '+e.message})}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
loadGW();setInterval(loadGW,15000);
</script>
</div>
)HTML";
    page += htmlFoot("/info");
    http.send(200, "text/html", page);
}

// ========== Save / Reset ==========
static void handleSave() {
    if (!checkAuth()) return;
    if (!checkCsrf()) return;
    WBConfig& cfg = configMgr.mut();
    cfg.wifiSSID = http.arg("wifi_ssid"); cfg.wifiPass = http.arg("wifi_pass");
    cfg.mqttHost = http.arg("mqtt_host"); cfg.mqttPort = http.arg("mqtt_port").toInt();
    cfg.mqttUser = http.arg("mqtt_user"); cfg.mqttPass = http.arg("mqtt_pass");
    cfg.mqttClientId = http.arg("mqtt_cid");
    cfg.authEnabled = http.arg("auth_en") == "1";
    cfg.authUser = http.arg("auth_user"); cfg.authPass = http.arg("auth_pass");
    cfg.bleAddr = normalizeMAC(http.arg("ble_addr")); cfg.blePin = http.arg("ble_pin");
    cfg.bleService = http.arg("ble_svc"); cfg.bleChar = http.arg("ble_chr");
    cfg.statusPollMs = http.arg("poll_status").toInt();
    cfg.realtimePollMs = http.arg("poll_rt").toInt();
    cfg.haDiscoveryPrefix = http.arg("ha_prefix"); cfg.haDeviceId = http.arg("ha_devid");
    if (cfg.mqttPort == 0) cfg.mqttPort = 1883;
    if (cfg.statusPollMs < 1000) cfg.statusPollMs = 10000;
    if (cfg.realtimePollMs < 1000) cfg.realtimePollMs = 30000;
    configMgr.save();

    String page = htmlHead("Saved");
    page += "<div class='card' style='text-align:center'><h2 style='color:var(--success)'>&#x2705; Saved!</h2>";
    page += "<p style='color:var(--text2);margin-top:10px'>Rebooting...</p><div class='spinner' style='margin:16px auto'></div></div>";
    page += "</div></body></html>";
    http.send(200, "text/html", page);
    webServer.requestReboot();
}

static void handleReset() {
    if (!checkAuth()) return;
    if (!checkCsrf()) return;
    configMgr.reset();
    String page = htmlHead("Reset");
    page += "<div class='card' style='text-align:center'><h2 style='color:var(--warning)'>Factory Reset</h2>";
    page += "<p style='color:var(--text2)'>Rebooting into AP mode...</p></div></div></body></html>";
    http.send(200, "text/html", page);
    webServer.requestReboot();
}

static void handleNotFound() {
    // In AP mode, redirect to captive portal
    if (WiFi.getMode() == WIFI_AP_STA || WiFi.getMode() == WIFI_AP) {
        http.sendHeader("Location", "http://192.168.4.1/config", true);
        http.send(302, "text/plain", "");
        return;
    }
    // In STA mode, show styled 404 page
    String page = htmlHead("Not Found");
    page += "<div style='text-align:center;padding:60px 20px'>";
    page += "<div style='font-size:4em;color:var(--text3);margin-bottom:10px'>404</div>";
    page += "<h2 style='color:var(--text2)'>Page Not Found</h2>";
    page += "<p style='color:var(--text3);margin:16px 0'>The path '";
    page += http.uri();
    page += "' doesn't exist.</p>";
    page += "<a href='/' class='btn btn-success' style='display:inline-block;text-decoration:none;padding:12px 24px'>Back to Dashboard</a>";
    page += "</div>";
    page += htmlFoot("/");
    http.send(404, "text/html", page);
}

// ========== Server setup ==========
// ========== Web OTA Upload ==========
// ========== PAGE: Sessions Heatmap (/sessions) ==========
static void handleSessionsPage() {
    String page = htmlHead("Sessions");
    page += R"HTML(
<div class='loading' id='ld'><div class='ld-spin'></div>Loading...</div>
<div id='pg' style='display:none'>
<h1>&#x1F4CA; Charging Sessions</h1>
<p class='subtitle'>Weekly pattern from last 30 days</p>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4C5;</span><h2>Weekly Heatmap</h2></div>
  <div style='overflow-x:auto;-webkit-overflow-scrolling:touch;margin:0 -4px;padding:0 4px'>
    <div id='heatmap' style='display:grid;grid-template-columns:48px repeat(24,minmax(10px,1fr));gap:2px;font-size:.7em;min-width:320px'></div>
  </div>
  <div style='display:flex;justify-content:space-between;margin-top:10px;font-size:.78em;color:var(--text3)'>
    <span>Low</span>
    <div style='display:flex;gap:3px'>
      <div style='width:18px;height:12px;background:var(--elevated);border-radius:2px'></div>
      <div style='width:18px;height:12px;background:rgba(59,130,246,.2);border-radius:2px'></div>
      <div style='width:18px;height:12px;background:rgba(59,130,246,.5);border-radius:2px'></div>
      <div style='width:18px;height:12px;background:var(--primary);border-radius:2px'></div>
      <div style='width:18px;height:12px;background:#1d4ed8;border-radius:2px'></div>
    </div>
    <span>High</span>
  </div>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4C8;</span><h2>Recent Sessions</h2></div>
  <div id='slist'><span class='spinner'></span> Loading sessions...</div>
</div>

<p style='text-align:center;margin-top:16px'>
  <a href='/settings' style='color:var(--text3)'>&#x2190; Back to Settings</a>
</p>
</div>
<script>
var DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
function buildHeatmap(sessions){
  // day[0-6] × hour[0-23] sum of kWh
  var grid=[];for(var d=0;d<7;d++){grid.push([]);for(var h=0;h<24;h++)grid[d].push(0)}
  var max=0;
  sessions.forEach(function(s){
    if(!s.ts||!s.en)return;
    var dt=new Date(s.ts*1000);var day=dt.getDay();var hr=dt.getHours();
    var kwh=s.en/100;grid[day][hr]+=kwh;if(grid[day][hr]>max)max=grid[day][hr];
  });
  var hm=document.getElementById('heatmap');hm.innerHTML='';
  // Header row (empty corner + hour labels)
  hm.innerHTML+='<div></div>';
  for(var h=0;h<24;h++)hm.innerHTML+="<div style='text-align:center;color:var(--text3)'>"+(h%6===0?h:'')+"</div>";
  // Day rows
  for(var d=0;d<7;d++){
    hm.innerHTML+="<div style='font-weight:600;color:var(--text2);padding-right:4px'>"+DAYS[d]+"</div>";
    for(var h=0;h<24;h++){
      var v=grid[d][h];var intensity=max>0?v/max:0;
      var bg='var(--elevated)';
      if(intensity>0.75)bg='#1d4ed8';
      else if(intensity>0.5)bg='var(--primary)';
      else if(intensity>0.25)bg='rgba(59,130,246,.5)';
      else if(intensity>0)bg='rgba(59,130,246,.2)';
      hm.innerHTML+="<div title='"+DAYS[d]+" "+h+":00 — "+v.toFixed(1)+" kWh' style='aspect-ratio:1;background:"+bg+";border-radius:2px'></div>";
    }
  }
}
function loadSessions2(){
  fetch('/api/command?action=bapi&met=r_ses&par=null',{signal:AbortSignal.timeout(15000)})
    .then(function(x){return x.json()}).then(function(d){
      if(!d.r||!d.r.last){document.getElementById('slist').innerHTML='No sessions yet';return}
      var last=d.r.last,count=Math.min(30,last);
      var sessions=[],idx=0,start=last-count+1;
      function next(){
        if(idx>=count){
          buildHeatmap(sessions);
          var html='';sessions.slice().reverse().slice(0,10).forEach(function(s){
            var dur=s.dur?Math.round(s.dur/60)+'m':'-';
            var en=s.en?(s.en/100).toFixed(2)+' kWh':'-';
            var ts=s.ts?new Date(s.ts*1000).toLocaleString():'#'+s.id;
            html+="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:4px 0;display:flex;justify-content:space-between'><div><div style='font-size:.82em'>"+ts+"</div><div style='font-size:.78em;color:var(--text3)'>#"+s.id+" \u00B7 "+dur+"</div></div><div style='font-weight:600'>"+en+"</div></div>";
          });
          document.getElementById('slist').innerHTML=html||'No detail data';
          return;
        }
        var sid=start+idx;
        document.getElementById('slist').innerHTML="<span class='spinner'></span> "+idx+" / "+count;
        fetch('/api/command?action=bapi&met=r_log&par='+sid,{signal:AbortSignal.timeout(8000)})
          .then(function(x){return x.json()}).then(function(sd){
            if(sd.r){sessions.push({id:sid,dur:sd.r.dur||sd.r.duration,en:sd.r.en||sd.r.energy,ts:sd.r.ts||sd.r.timestamp||sd.r.start})}
            idx++;next();
          }).catch(function(){idx++;next()});
      }
      next();
    }).catch(function(e){document.getElementById('slist').innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'});
}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
loadSessions2();
</script>
)HTML";
    page += htmlFoot("/settings");
    http.send(200, "text/html", page);
}

static void handleOtaPage() {
    if (!checkAuth()) return;
    String page = htmlHead("Firmware Update");
    page += R"HTML(
<div class='loading' id='ld'><div class='ld-spin'></div>Loading...</div>
<div id='pg' style='display:none'>
<h1>&#x1F4E6; Firmware Update</h1>
<div class='card'>
  <p style='color:var(--text2);margin-bottom:16px'>Upload a .bin firmware file to update the gateway over WiFi.</p>
  <div style='background:rgba(34,197,94,.08);border:1px solid rgba(34,197,94,.2);border-radius:8px;padding:10px;margin-bottom:12px;font-size:.85em;color:#22c55e'>&#x1F512; Authenticated — OTA upload authorized</div>
  <input type='file' id='fw' accept='.bin' style='margin-bottom:12px'>
  <div id='ota-info' style='display:none;margin-bottom:12px'></div>
  <button class='btn btn-success' onclick='doOTA()'>Upload Firmware</button>
  <div id='ota-progress' style='display:none;margin-top:12px'>
    <div style='background:var(--border);border-radius:6px;overflow:hidden;height:8px'>
      <div id='ota-bar' style='background:var(--success);height:100%;width:0%;transition:width .3s'></div>
    </div>
    <p id='ota-status' style='text-align:center;margin-top:8px;color:var(--text2);font-size:.85em'>Uploading...</p>
  </div>
</div>
</div>
<script>
document.getElementById('fw').onchange=function(){var f=this.files[0];if(!f)return;var info=document.getElementById('ota-info');info.style.display='block';var sz=(f.size/1024).toFixed(0);var valid=f.name.endsWith('.bin')&&f.size>10000&&f.size<2000000;info.innerHTML=row('File',f.name)+row('Size',sz+' KB')+(valid?'':'<p style="color:var(--danger);margin-top:8px">Invalid: must be .bin, 10KB-2MB</p>')};
function doOTA(){var f=document.getElementById('fw').files[0];if(!f){toast('Select a firmware file','error');return}if(!f.name.endsWith('.bin')){toast('Must be a .bin file','error');return}if(f.size<10000||f.size>2000000){toast('File size invalid','error');return}var prog=document.getElementById('ota-progress');prog.style.display='block';var bar=document.getElementById('ota-bar');var stat=document.getElementById('ota-status');var xhr=new XMLHttpRequest();xhr.open('POST','/api/ota');xhr.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round(e.loaded/e.total*100);bar.style.width=pct+'%';stat.textContent='Uploading... '+pct+'%'}};xhr.onload=function(){if(xhr.status===200){stat.textContent='Update complete! Rebooting...';toast('Firmware updated!','success');bar.style.width='100%';bar.style.background='var(--success)'}else{stat.textContent='Failed: '+xhr.responseText;toast('Update failed','error');bar.style.background='var(--danger)'}};xhr.onerror=function(){stat.textContent='Upload failed';toast('Upload error','error')};xhr.send(f)}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
</script>
)HTML";
    page += htmlFoot("/info");
    http.send(200, "text/html", page);
}

bool otaInProgress = false;

static void handleOtaUpload() {
    HTTPUpload& upload = http.upload();
    static size_t totalSize = 0;
    static bool otaError = false;

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Upload start: %s\n", upload.filename.c_str());
        totalSize = 0;
        otaError = false;
        otaInProgress = true;

        // Check partition size
        const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
        if (partition) {
            Serial.printf("[OTA] Target partition: %s (%u bytes)\n", partition->label, partition->size);
        }

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial.printf("[OTA] Begin failed: %s\n", Update.errorString());
            otaError = true;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE && !otaError) {
        // First chunk — validate ESP32 magic byte
        if (totalSize == 0 && upload.currentSize > 0) {
            if (upload.buf[0] != 0xE9) {
                Serial.println("[OTA] REJECTED: not ESP32 firmware (magic byte != 0xE9)");
                Update.abort();
                otaError = true;
                return;
            }
            Serial.println("[OTA] Firmware magic byte OK");
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Serial.printf("[OTA] Write failed: %s\n", Update.errorString());
            otaError = true;
        }
        totalSize += upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        otaInProgress = false;
        if (otaError) {
            Update.abort();
            Serial.println("[OTA] Aborted due to errors");
            http.send(500, "text/plain", "Upload failed");
        } else if (Update.end(true)) {
            Serial.printf("[OTA] Success! %u bytes written to partition\n", totalSize);
            http.send(200, "text/plain", "OK");
            delay(1000);
            ESP.restart();
        } else {
            Serial.printf("[OTA] End failed: %s\n", Update.errorString());
            http.send(500, "text/plain", Update.errorString());
        }
    }
}

// PWA manifest — lets users "Install" the web app on their home screen
static void handleManifest() {
    http.sendHeader("Cache-Control", "public, max-age=86400");
    http.send(200, "application/manifest+json",
        "{\"name\":\"Wallbox Gateway\",\"short_name\":\"Wallbox\","
        "\"display\":\"standalone\",\"orientation\":\"portrait\","
        "\"background_color\":\"#0f1117\",\"theme_color\":\"#0f1117\","
        "\"start_url\":\"/\",\"scope\":\"/\","
        "\"icons\":[{"
        "\"src\":\"data:image/svg+xml;utf8,"
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 192 192'>"
        "<rect width='192' height='192' rx='40' fill='%230f1117'/>"
        "<path d='M104 36 L60 104 L92 104 L88 156 L132 88 L100 88 L104 36 Z' fill='%233b82f6'/>"
        "</svg>\","
        "\"sizes\":\"192x192\",\"type\":\"image/svg+xml\",\"purpose\":\"any\"}]}");
}

// Minimal service worker — forces cache purge on install, no caching of resources
static void handleServiceWorker() {
    http.sendHeader("Cache-Control", "no-cache");
    http.send(200, "application/javascript",
        "self.addEventListener('install',function(e){e.waitUntil(caches.keys().then(function(keys){return Promise.all(keys.map(function(k){return caches.delete(k)}))}).then(function(){return self.skipWaiting()}))});"
        "self.addEventListener('activate',function(e){e.waitUntil(self.clients.claim())});"
        "self.addEventListener('fetch',function(e){e.respondWith(fetch(e.request,{cache:'no-cache'}))});");
}

static void registerRoutes() {
    http.on("/style.css", handleStyleCss);
    http.on("/app.js", handleAppJs);
    http.on("/save", HTTP_POST, handleSave);
    http.on("/reset", HTTP_POST, handleReset);
    http.on("/api/ble-scan", handleBleScan);
    http.on("/api/wifi-scan", handleWifiScan);
    http.on("/api/status", handleApiStatus);
    http.on("/api/charger", handleApiCharger);
    http.on("/api/command", handleApiCommand);
    http.on("/ota", handleOtaPage);
    http.on("/api/ota", HTTP_POST, []() {}, handleOtaUpload);
    http.on("/sessions", handleSessionsPage);
    http.on("/manifest.json", handleManifest);
    http.on("/sw.js", handleServiceWorker);
    http.on("/favicon.ico", []() { http.send(204); });
}

void WBWebServer::beginAP() {
    _apMode = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[Web] AP: %s (pass: %s) IP: %s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    dns.start(53, "*", WiFi.softAPIP());

    registerRoutes();
    http.on("/", handleConfig);  // AP mode: config first
    http.on("/config", handleConfig);
    http.on("/dashboard", handleDashboard);
    http.on("/settings", handleSettings);
    http.on("/info", handleInfo);
    http.onNotFound(handleNotFound);
    http.begin();
    Serial.println("[Web] AP captive portal ready");
}

void WBWebServer::beginSTA() {
    _apMode = false;
    registerRoutes();
    http.on("/", handleDashboard);  // STA mode: dashboard is home
    http.on("/config", handleConfig);
    http.on("/settings", handleSettings);
    http.on("/info", handleInfo);
    http.begin();
    Serial.printf("[Web] http://%s/ (dashboard)\n", WiFi.localIP().toString().c_str());
}

void WBWebServer::loop() {
    if (_apMode) dns.processNextRequest();
    http.handleClient();
    if (_rebootRequested) {
        static uint32_t rt = 0;
        if (rt == 0) rt = millis();
        if (millis() - rt > 2000) { Serial.println("[Web] Rebooting..."); ESP.restart(); }
    }
}
