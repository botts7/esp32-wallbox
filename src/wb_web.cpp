#include "wb_web.h"
#include "wb_config.h"
#include "wb_ble.h"
#include "wb_log.h"
#include "wb_version.h"
#include "wb_health.h"
#include "wb_ota_history.h"
#include "wb_diag.h"
#include "wb_watchdog.h"
#include "bapi.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
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
            Log.printf("[Auth] LOCKED OUT after %d failures\n", authFailCount);
        } else {
            delay(1000);  // 1s delay per failure — slows brute force
            Log.printf("[Auth] Failed %d/5\n", authFailCount);
        }
        http.requestAuthentication();
        return false;
    }
    authFailCount = 0;
    return true;
}

// ========== CSS ==========
static void handleStyleCss() {
    // Aggressive caching is safe — URL carries ?v=<buildVer> so cache
    // is naturally busted on upgrade. Was no-cache → every navigation
    // triggered a conditional GET (304 round-trip) that piled on TCP
    // sockets alongside in-flight BAPI calls. Under heavy /settings
    // load the resulting heap pressure was crashing the gateway
    // (heap_min_ever observed dipping to ~59 KB; panic threshold is
    // ~30 KB). immutable tells the browser to skip revalidation
    // entirely until the URL hash changes.
    http.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    http.send(200, "text/css", R"CSS(
:root{--bg:#0f1117;--surface:#1a1d28;--card:#1a1d28;--elevated:#232736;--primary:#3b82f6;--success:#22c55e;--danger:#ef4444;--warning:#f59e0b;--text:#e2e8f0;--text2:#94a3b8;--text3:#64748b;--border:#2a2d3a;--accent:#4fc3f7}
@media (prefers-color-scheme:light){:root:not([data-theme]){--bg:#f5f7fa;--surface:#ffffff;--card:#ffffff;--elevated:#eef2f7;--text:#1e293b;--text2:#475569;--text3:#64748b;--border:#d8dfe8;--accent:#1d4ed8}}
:root[data-theme="light"]{--bg:#f5f7fa;--surface:#ffffff;--card:#ffffff;--elevated:#eef2f7;--text:#1e293b;--text2:#475569;--text3:#64748b;--border:#d8dfe8;--accent:#1d4ed8}
:root[data-theme="dark"]{--bg:#0f1117;--surface:#1a1d28;--card:#1a1d28;--elevated:#232736;--text:#e2e8f0;--text2:#94a3b8;--text3:#64748b;--border:#2a2d3a;--accent:#4fc3f7}
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
    // See handleStyleCss() — same rationale. ?v=<buildVer> in the URL
    // is the cache-bust signal; immutable means no re-validation
    // round-trip on subsequent navigations within the same firmware.
    http.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    http.send(200, "application/javascript", R"JS(
function toast(msg,type){type=type||'info';var c=document.getElementById('toast-c');if(!c){c=document.createElement('div');c.id='toast-c';c.className='toast-container';document.body.appendChild(c)}var t=document.createElement('div');t.className='toast toast-'+type;t.textContent=msg;c.appendChild(t);setTimeout(function(){t.style.opacity='0';t.style.transition='opacity .3s';setTimeout(function(){t.remove()},300)},3000)}
function confirm2(msg,cb){var o=document.createElement('div');o.style.cssText='position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:300;display:flex;align-items:center;justify-content:center';o.innerHTML="<div style='background:#1a1d28;border:1px solid #2a2d3a;border-radius:14px;padding:24px;max-width:320px;text-align:center'><p style='margin:0 0 16px;color:#e2e8f0'>"+msg+"</p><div style='display:flex;gap:10px'><button id='cf-cancel' style='flex:1;padding:12px;border-radius:8px;border:1px solid #2a2d3a;background:transparent;color:#94a3b8;cursor:pointer'>Cancel</button><button id='cf-ok' style='flex:1;padding:12px;border-radius:8px;border:none;background:#ef4444;color:#fff;cursor:pointer'>Confirm</button></div></div>";document.body.appendChild(o);document.getElementById('cf-cancel').onclick=function(){o.remove()};document.getElementById('cf-ok').onclick=function(){o.remove();cb()};o.onclick=function(e){if(e.target===o)o.remove()}}
function selectDevice(addr,ev){document.getElementById('ble_addr').value=addr;document.querySelectorAll('.scan-result').forEach(function(e){e.style.borderColor=''});if(ev&&ev.currentTarget)ev.currentTarget.style.borderColor='var(--primary)'}
function formatMAC(i){var v=i.value.replace(/[^0-9a-fA-F]/g,'').toUpperCase(),f='';for(var j=0;j<v.length&&j<12;j++){if(j>0&&j%2===0)f+=':';f+=v[j]}i.value=f}
function startScan(){var b=document.getElementById('scanBtn'),r=document.getElementById('scanResults');b.disabled=true;b.innerHTML="<span class='spinner'></span>Scanning...";r.innerHTML="<div style='text-align:center;padding:16px;color:var(--text3)'><span class='spinner'></span> Scanning...</div>";fetch('/api/ble-scan').then(function(x){return x.json()}).then(function(d){b.disabled=false;b.innerHTML='Scan for Chargers';r.replaceChildren();if(!d.devices.length){var n=document.createElement('div');n.style.cssText='text-align:center;padding:16px;color:var(--text3)';n.textContent='No devices found';r.appendChild(n);return}/* SECURITY: build DOM nodes via createElement+textContent — BLE device name and address are attacker-controllable (any radio in range can advertise arbitrary strings) and previously flowed into innerHTML, an XSS sink that ran in admin auth context. */d.devices.forEach(function(v){var row=document.createElement('div');row.className=v.is_wallbox?'scan-result wb':'scan-result';row.addEventListener('click',function(ev){selectDevice(v.addr,ev)});var left=document.createElement('div');var name=document.createElement('span');name.className='scan-name';name.textContent=v.name||'Unknown';left.appendChild(name);if(v.is_wallbox){var bg=document.createElement('span');bg.className='badge badge-success';bg.textContent='WALLBOX';bg.style.marginLeft='8px';left.appendChild(bg)}left.appendChild(document.createElement('br'));var addr=document.createElement('span');addr.className='scan-addr';addr.textContent=v.addr;left.appendChild(addr);row.appendChild(left);var rssi=document.createElement('span');rssi.className='scan-rssi';rssi.textContent=v.rssi+' dBm';row.appendChild(rssi);r.appendChild(row)})}).catch(function(e){b.disabled=false;b.innerHTML='Scan for Chargers';r.replaceChildren();var er=document.createElement('div');er.style.color='var(--danger)';er.textContent=String(e);r.appendChild(er)})}
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
    r.replaceChildren();
    if(d.error){var er=document.createElement('div');er.style.cssText='padding:10px;color:var(--danger)';er.textContent='Scan error: '+d.error;r.appendChild(er);return}
    if(!d.networks||!d.networks.length){var no=document.createElement('div');no.style.cssText='padding:10px;text-align:center;color:var(--text3)';no.textContent='No networks found';r.appendChild(no);return}
    /* SECURITY: SSIDs are attacker-controllable (any AP can broadcast arbitrary
       SSID strings); building via createElement+textContent prevents XSS that
       would otherwise run in the WiFi-setup admin context. */
    d.networks.sort(function(a,b){return b.rssi-a.rssi}).forEach(function(n){
      var row=document.createElement('div');
      row.style.cssText='padding:10px 12px;border-radius:6px;cursor:pointer;display:flex;justify-content:space-between;align-items:center';
      row.addEventListener('click',function(){pickSsid(n.ssid)});
      var nameSpan=document.createElement('span');
      nameSpan.style.fontWeight='500';
      nameSpan.textContent=n.ssid;
      row.appendChild(nameSpan);
      var rssiSpan=document.createElement('span');
      rssiSpan.style.cssText='color:var(--text3);font-size:.8em';
      rssiSpan.textContent=n.rssi+' dBm';
      row.appendChild(rssiSpan);
      r.appendChild(row);
    });
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

// SVG icons for nav (inline, no external deps).
// Explicit width/height attrs guarantee correct sizing even if
// /style.css is slow / stale-cached on first paint. CSS can still
// override these via the `.nav-item svg` rule.
#define SVG_DASHBOARD "<svg width='22' height='22' viewBox='0 0 24 24'><path d='M3 13h8V3H3v10zm0 8h8v-6H3v6zm10 0h8V11h-8v10zm0-18v6h8V3h-8z'/></svg>"
#define SVG_SETTINGS  "<svg width='22' height='22' viewBox='0 0 24 24'><path d='M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.07.62-.07.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z'/></svg>"
#define SVG_CONFIG    "<svg width='22' height='22' viewBox='0 0 24 24'><path d='M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z'/></svg>"
#define SVG_INFO      "<svg width='22' height='22' viewBox='0 0 24 24'><path d='M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z'/></svg>"
#define SVG_LOGS      "<svg width='22' height='22' viewBox='0 0 24 24'><path d='M14 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6zm2 16H8v-2h8v2zm0-4H8v-2h8v2zm-3-5V3.5L18.5 9H13z'/></svg>"

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
        "html{background:#0f1117}"
        "body{margin:0;padding:16px;padding-bottom:80px;background:#0f1117;color:#e2e8f0;font-family:-apple-system,system-ui,sans-serif;min-height:100vh;visibility:hidden}"
        "body.ready{visibility:visible}"
        "html[data-theme=\"light\"],html[data-theme=\"light\"] body{background:#f5f7fa;color:#1e293b}"
        "@media (prefers-color-scheme:light){html:not([data-theme]),html:not([data-theme]) body{background:#f5f7fa;color:#1e293b}}"
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
        // Boot overlay — covers the page while the gateway is still
        // coming up (BLE handshake/init takes ~3–8s after a reboot).
        // Prevents users from navigating, firing BAPI calls, or
        // hammering refresh during the window where requests would
        // return "BLE not connected" or queue against a busy mutex.
        // Hidden as soon as WS pushes ble.state === 'connected'.
        ".wb-overlay{position:fixed;inset:0;background:rgba(15,17,23,.94);"
        "-webkit-backdrop-filter:blur(6px);backdrop-filter:blur(6px);"
        "z-index:500;display:none;align-items:center;justify-content:center;padding:20px}"
        ".wb-overlay.show{display:flex}"
        ".wb-overlay-card{background:#1a1d28;border:1px solid #2a2d3a;border-radius:14px;"
        "padding:24px 28px;max-width:380px;width:100%;text-align:center;"
        "box-shadow:0 16px 48px rgba(0,0,0,.5)}"
        ".wb-overlay-card h3{margin:0 0 6px;font-size:1.05em;color:#e2e8f0;font-weight:600}"
        ".wb-overlay-card p{margin:4px 0;color:#94a3b8;font-size:.85em}"
        ".wb-overlay-card .wb-overlay-hint{color:#64748b;font-size:.78em;margin-top:14px}"
        ".wb-overlay-spin{width:36px;height:36px;border:3px solid #2a2d3a;"
        "border-top-color:#3b82f6;border-radius:50%;animation:sp 1s linear infinite;margin:0 auto 14px}"
        ".wb-overlay-bar-bg{width:100%;height:8px;background:#2a2d3a;border-radius:4px;"
        "margin:14px 0 6px;overflow:hidden}"
        ".wb-overlay-bar{height:100%;background:linear-gradient(90deg,#3b82f6,#4fc3f7);"
        "width:5%;transition:width .4s ease;border-radius:4px}"
        "</style>";
    // Cache-bust CSS/JS with boot time (unique per firmware build + boot)
    static String buildVer;
    if (buildVer.length() == 0) {
        buildVer = String((uint32_t)(esp_random() & 0xFFFFFF), HEX);
    }
    h += "<link rel='stylesheet' href='/style.css?v=" + buildVer + "'>"
        "<script src='/app.js?v=" + buildVer + "' defer></script>"
        // Fetch limiter — ESP32's web server has very limited concurrent
        // connection slots. Without this, pages that fire multiple
        // overlapping fetches (loadGW + loadOtaHistory + loadBootReason
        // + loadDiag + loadNotifs + updateBleHealth + the page's own
        // BAPI calls + WebSocket) easily flood the server, causing
        // ERR_EMPTY_RESPONSE, /app.js stalls, and eventually panic
        // crashes. We cap concurrency at 2 in-flight requests at any
        // time; the rest queue. Per-request latency stays unchanged on
        // the happy path; under load the queue holds requests until
        // a slot is free.
        "<script>(function(){var maxC=1,inflight=0,q=[];var nf=window.fetch.bind(window);"
          // Universal retry policy for /api/command:
          //   - 429 (rate-limited) → respect Retry-After
          //   - 503 (busy) → respect Retry-After (default 1.2s)
          //   - 200 with {error} or null/missing r → backoff retry
          //   Backoff: 800ms, 2.4s, 7.2s (3x). Max 3 attempts total.
          //   Non-/api/command URLs are returned as-is (no body parse).
          "function isCmd(u){return typeof u==='string'&&u.indexOf('/api/command')>=0}"
          "function delayFor(t,fallback){return new Promise(function(r){setTimeout(r,t.attempt===0?800:t.attempt===1?2400:fallback||7200)})}"
          "function pump(){while(inflight<maxC&&q.length){var t=q.shift();inflight++;"
            "nf(t.url,t.opts).then(function(r){inflight--;"
              "if(!isCmd(t.url)){t.res(r);pump();return}"
              "var status=r.status;"
              "if((status===429||status===503)&&t.attempt<2){"
                "var ra=parseFloat(r.headers.get('Retry-After'));"
                "var wait=isNaN(ra)?(t.attempt===0?800:2400):ra*1000;"
                "t.attempt++;setTimeout(function(){q.push(t);pump()},wait);pump();return"
              "}"
              "if(status===200&&t.attempt<2){"
                // peek at body via clone — if {r: null|undefined} or {error}
                // signals a transient BAPI-side miss, retry with backoff.
                // Pass the real Response through if body looks good or
                // if we've exhausted retries.
                "r.clone().json().then(function(d){"
                  "var hasErr=d&&typeof d==='object'&&'error' in d;"
                  "var nullR=d&&typeof d==='object'&&'r' in d&&(d.r===null||d.r===undefined);"
                  "if(hasErr||nullR){t.attempt++;delayFor(t).then(function(){q.push(t);pump()});pump();return}"
                  "t.res(r);pump()"
                "}).catch(function(){t.res(r);pump()});"
                "return"
              "}"
              "t.res(r);pump()"
            "},function(e){inflight--;"
              "if(isCmd(t.url)&&t.attempt<2){t.attempt++;delayFor(t).then(function(){q.push(t);pump()});return}"
              "t.rej(e);pump()"
            "})}}"
          "window.fetch=function(url,opts){return new Promise(function(res,rej){"
            "q.push({url:url,opts:opts,res:res,rej:rej,attempt:0});pump()})};"
        "})();</script>"
        // (Note: 429/503 retry-after-honoring logic now lives INSIDE the
        // single-flight limiter above, courtesy of the reentrancy-fix
        // patch. Dropped my separate wrapper that was here — it would
        // double-wrap fetch() and double-count retries.)
        "<script>(function(){try{var t=localStorage.getItem('wb-theme');if(t==='light'||t==='dark')document.documentElement.setAttribute('data-theme',t)}catch(e){}})();</script>"
        "<script>(function(){var handlers={};var sock=null;var rd=1000;var open=false;function connect(){try{sock=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.hostname+':81/');sock.onopen=function(){open=true;rd=1000;document.documentElement.setAttribute('data-ws','1')};sock.onmessage=function(e){try{var m=JSON.parse(e.data);var hs=handlers[m.t];if(hs){for(var i=0;i<hs.length;i++){try{hs[i](m.d,m)}catch(ee){}}}}catch(err){}};sock.onclose=function(){open=false;document.documentElement.removeAttribute('data-ws');sock=null;setTimeout(connect,rd);rd=Math.min(rd*2,30000)};sock.onerror=function(){}}catch(e){setTimeout(connect,rd)}}window.wbws={subscribe:function(t,fn){(handlers[t]=handlers[t]||[]).push(fn)},isOpen:function(){return open},send:function(s){if(sock&&open)sock.send(s)}};connect();})();</script>"
        "<script>(function(){var _lastSt=null;function load(){var def={enabled:false,events:{started:true,complete:true,paused:false,error:true,plug_in:false,plug_out:false}};try{var s=JSON.parse(localStorage.getItem('wb-notif')||'null');if(!s)return def;if(!s.events)s.events=def.events;return s}catch(e){return def}}function fire(title,body){try{new Notification(title,{body:body,tag:'wb',silent:false})}catch(e){}}window.wbFireNotif=fire;window.wbCheckStatus=function(s){if(!s||typeof s.st!=='number')return;var st=s.st;if(_lastSt===null){_lastSt=st;return}if(st===_lastSt)return;var N=load();var was=_lastSt;_lastSt=st;if(!N.enabled||typeof Notification==='undefined'||Notification.permission!=='granted')return;var charging=[2,20,179],complete=[21],paused=[3,22,178,193],error=[6,14],ready=[0,7,16,161,189],connected=[1,13,17];function inG(v,g){return g.indexOf(v)>=0}if(inG(st,complete)&&N.events.complete)fire('Charging complete','Your car is ready to go.');else if(inG(st,charging)&&!inG(was,charging)&&N.events.started)fire('Charging started','Active.');else if(inG(st,paused)&&!inG(was,paused)&&N.events.paused)fire('Charging paused','See dashboard.');else if(inG(st,error)&&!inG(was,error)&&N.events.error)fire('Charger error','See dashboard.');else if(inG(st,connected)&&inG(was,ready)&&N.events.plug_in)fire('Plug connected','Ready to charge.');else if(inG(st,ready)&&!inG(was,ready)&&N.events.plug_out)fire('Plug disconnected','Charger idle.')};})();</script>"
        "</head><body>";
    // Boot overlay markup — only rendered when there's a BLE link we'd be
    // waiting on. Three exclusion cases caught here:
    //   1. AP / captive-portal mode (no STA WiFi yet) — BLE task never starts.
    //   2. No charger address configured (STA up but bleAddr blank) — same.
    //   3. We're serving /config or /ota in setup flow — even if BLE is
    //      configured, the user is here to change config, don't gate them.
    //
    // peter-mcc hit case 1 on a fresh USB-flash of rc21 (#4): the AP-mode
    // setup page rendered with the overlay's `show` class because BLE
    // wasn't "connected", and there was no BLE task to push a `connected`
    // event over WS to dismiss it — page appeared permanently stuck.
    // Rendering the overlay HTML at all in setup mode means the JS that
    // follows could still re-show it on a WS-drop watchdog tick. Cleanest
    // fix: omit the overlay markup entirely when we know there's no BLE
    // to wait for.
    bool inSetupMode = webServer.isAPMode() || !configMgr.hasBLE();
    bool bootReady = (bleState == "connected");
    if (!inSetupMode) {
        h += "<div id='wb-boot-overlay' class='wb-overlay";
        h += bootReady ? "" : " show";
        h += "'><div class='wb-overlay-card'>"
             "<div class='wb-overlay-spin'></div>"
             // Title and subtitle are both JS-controlled — they say
             // "Wallbox Gateway is starting" only during an actual cold
             // boot / BLE-disconnected state. Navigation transitions show
             // "Loading…" and WS-drop reconnections show "Reconnecting…"
             // so users don't see a misleading boot screen during a
             // quick page change.
             "<div id='wb-boot-title' style='font-size:1.05em;font-weight:600;color:#e2e8f0;margin:0 0 6px'>"
             "Wallbox Gateway is starting</div>"
             "<p id='wb-boot-stage'>Initializing</p>"
             "<div class='wb-overlay-bar-bg'><div id='wb-boot-bar' class='wb-overlay-bar'></div></div>"
             "<p id='wb-boot-hint' class='wb-overlay-hint'>This usually takes 5&ndash;15 seconds after a reboot.</p>"
             "</div></div>";
    }
    h += "<div class='container'>"
        "<div class='ble-bar'><span class='ble-dot'></span>BLE: <span id='ble-bar-state'>";
    h += bleState;
    h += "</span>";
    h += "<span id='ble-bar-rssi'>";
    if (bleOk && rssi > -127) {
        h += " (" + String(rssi) + " dBm)";
    }
    h += "</span></div>";
    // Keep the banner live — subscribe to the same 'ble' WS push the page
    // bodies use, so banner + Gateway-card BLE Signal always agree.
    // Also drive the boot overlay: state→progress map below mirrors the
    // sequence in WallboxBLE::stateStr(). When connected, overlay fades
    // out after a short delay so the user sees a 100% finish.
    h += "<script>(function(){"
         "var O=document.getElementById('wb-boot-overlay');"
         "var B=document.getElementById('wb-boot-bar');"
         "var S=document.getElementById('wb-boot-stage');"
         "var T=document.getElementById('wb-boot-title');"
         "var H=document.getElementById('wb-boot-hint');"
         // Overlay has three independent modes; the active one decides
         // what title/hint is shown. WS BLE pushes update the progress
         // bar + stage text regardless, but title/hint are only
         // touched in 'boot' mode so a click-nav 'Loading' or
         // watchdog 'Reconnecting' isn't clobbered when the next ble
         // event arrives.
         "var mode='boot';"
         "var M={'disconnected':{p:20,t:'Searching for charger\xE2\x80\xA6'},"
                 "'connecting':{p:50,t:'Connecting to charger\xE2\x80\xA6'},"
                 "'authenticating':{p:75,t:'Authenticating\xE2\x80\xA6'},"
                 "'connected':{p:100,t:'Ready'},"
                 "'error':{p:15,t:'Retrying\xE2\x80\xA6'},"
                 "'unknown':{p:10,t:'Starting up\xE2\x80\xA6'}};"
         "var BOOT_TITLE='Wallbox Gateway is starting';"
         // 5–15 uses a JS Unicode escape (parsed by the browser)
         // rather than the equivalent \xE2\x80\x93 C++ hex escape,
         // which the compiler ate as one variable-length escape
         // (\x9315) and produced garbage bytes in the binary.
         "var BOOT_HINT='This usually takes 5\\u201315 seconds after a reboot.';"
         "function show(){if(O)O.classList.add('show')}"
         "function hide(){if(O)O.classList.remove('show')}"
         "if(window.wbws){window.wbws.subscribe('ble',function(d){"
             "var s=document.getElementById('ble-bar-state');if(s)s.textContent=d.state;"
             "var r=document.getElementById('ble-bar-rssi');if(r)r.textContent=(d.state==='connected'&&d.rssi>-127)?(' ('+d.rssi+' dBm)'):'';"
             "var m=M[d.state]||M['disconnected'];"
             // bar + stage track real BLE state in all modes; the
             // title/hint only follow BLE state in boot mode.
             "if(B)B.style.width=m.p+'%';"
             "if(mode==='boot'){"
                 "if(S)S.textContent=m.t;"
                 "if(T)T.textContent=BOOT_TITLE;"
                 "if(H)H.textContent=BOOT_HINT;"
             "}"
             "if(d.state==='connected'){"
                 // 100% then fade out — reset mode so the next time
                 // the overlay reappears (e.g. after a reboot) it
                 // starts fresh in boot mode.
                 "setTimeout(function(){hide();mode='boot'},600)"
             "}else if(mode==='boot'){show()}"
         "});}"
         // Watchdog: only fires after WS has been confirmed open at
         // least once (wasOpen starts false). A genuine open→closed
         // transition means the gateway disappeared — switch to
         // 'reconnect' mode so subsequent BLE pushes don't reset the
         // title back to 'Wallbox Gateway is starting'.
         "var wasOpen=false;setInterval(function(){"
             "var on=window.wbws&&window.wbws.isOpen();"
             "if(on)wasOpen=true;"
             "else if(wasOpen){"
                 "mode='reconnect';"
                 "if(T)T.textContent='Reconnecting to gateway';"
                 "if(S)S.textContent='Please wait\xE2\x80\xA6';"
                 "if(H)H.textContent='The gateway will return in a few seconds.';"
                 "if(B)B.style.width='5%';"
                 "show()"
             "}"
         "},1500);"
         // Nav-click loading hint — switch into 'nav' mode so the WS
         // 'ble' handler above won't overwrite the title back to the
         // boot string. 150ms deferred show: fast page transitions
         // (paint-hold or cache-hit nav) finish before the timer
         // fires, so the user never sees a flash of overlay. Slow
         // navs (cold-cache HTML download + parse) fire the overlay.
         // The browser tearing down the page on navigation cancels
         // pending setTimeout, so a successful fast nav guarantees
         // the timer never runs.
         "document.addEventListener('click',function(e){"
             "var t=e.target;while(t&&t.nodeName!=='A')t=t.parentNode;"
             "if(!t||!t.href||t.target||t.host!==location.host)return;"
             "if(t.getAttribute('href').charAt(0)==='#')return;"
             "setTimeout(function(){"
                 "mode='nav';"
                 "if(T)T.textContent='Loading';"
                 "if(S)S.textContent='Opening page\xE2\x80\xA6';"
                 "if(H)H.textContent='';"
                 "if(B)B.style.width='40%';"
                 "show();"
             "},150);"
         "});"
         "})();</script>";
    return h;
}

static String htmlFoot(const char* activePath) {
    String h = "</div><nav class='nav-bar'>";
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
    navItem("/logs", SVG_LOGS, "Logs");
    h += "</nav>"
         "<script>if('serviceWorker' in navigator)navigator.serviceWorker.register('/sw.js').catch(function(){});</script>"
         "<script>(function(){var r=function(){document.body.classList.add('ready')};if(document.readyState==='interactive'||document.readyState==='complete')r();else document.addEventListener('DOMContentLoaded',r)})();</script>"
         "</body></html>";
    return h;
}

// ========== API endpoints ==========

static void handleBleScan() {
    // Don't scan if BLE is actively connecting — causes conflicts
    if (wallboxBLE.state() == WallboxBLE::State::CONNECTING ||
        wallboxBLE.state() == WallboxBLE::State::AUTHENTICATING) {
        Log.println("[BLE-Scan] requested via web UI, but BLE is busy — refused");
        http.send(200, "application/json", "{\"devices\":[],\"busy\":true}");
        return;
    }
    Log.println("[BLE-Scan] requested via web UI, starting 8s scan...");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    NimBLEScanResults results = scan->start(8, false);
    Log.printf("[BLE-Scan] complete: %d device(s) seen\n", results.getCount());
    String json = "{\"devices\":[";
    bool first = true;
    for (int i = 0; i < results.getCount(); i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        String name = dev.getName().c_str();
        String addr = dev.getAddress().toString().c_str();
        int rssi = dev.getRSSI();
        bool isWB = name.startsWith("WB") || name.indexOf("allbox") >= 0;
        Log.printf("[BLE-Scan]   %s%s %s  RSSI:%d\n",
                   isWB ? "* " : "  ",
                   addr.c_str(),
                   name.length() ? name.c_str() : "(no name)",
                   rssi);
        if (!first) json += ",";
        first = false;
        json += "{\"addr\":\"" + addr + "\",\"name\":\"" + name + "\",\"rssi\":" + String(rssi) + ",\"is_wallbox\":" + (isWB ? "true" : "false") + "}";
    }
    json += "]}";
    http.send(200, "application/json", json);
}

static void handleWifiScan() {
    // Auth not required in AP mode (otherwise users can't pick a WiFi
    // during initial setup). When in STA mode (already connected),
    // require auth to avoid leaking nearby SSIDs to anyone on the LAN.
    if (WiFi.getMode() != WIFI_AP && !checkAuth()) return;
    // Ensure STA is enabled for scanning (AP-only mode can't scan)
    wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    else if (mode == WIFI_OFF) WiFi.mode(WIFI_STA);

    Log.println("[WiFi-Scan] requested via web UI, scanning...");
    int n = WiFi.scanNetworks(false, true, false, 400);
    if (n < 0) {
        Log.printf("[WiFi-Scan] failed (code %d)\n", n);
        http.send(500, "application/json", "{\"error\":\"scan failed\",\"code\":" + String(n) + "}");
        return;
    }
    Log.printf("[WiFi-Scan] complete: %d network(s) found\n", n);
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
    json += ",\"chg_sn\":\"" + wallboxBLE.chargerSerial() + "\"";
    json += ",\"chg_mac\":\"" + wallboxBLE.chargerMac() + "\"";
    json += ",\"chg_grounding\":\"" + wallboxBLE.chargerGrounding() + "\"";
    // chg_app_fw — charger application firmware (the version Wallbox app
    // shows), distinct from dev_fw which is the BLE module firmware.
    // chg_project — canonical model identifier (e.g. "prj15-pulsar-max").
    json += ",\"chg_app_fw\":\"" + wallboxBLE.chargerAppFirmware() + "\"";
    json += ",\"chg_project\":\"" + wallboxBLE.chargerProject() + "\"";
    json += ",\"chg_sessions\":" + String((int)wallboxBLE.chargerSessionCount());
    json += ",\"chg_power_boost\":" + String((int)wallboxBLE.chargerPowerBoost());
    json += ",\"chg_lock_state\":" + String((int)wallboxBLE.chargerLockState());
    json += ",\"chg_net_ssid\":\"" + wallboxBLE.chargerNetworkSsid() + "\"";
    json += ",\"chg_net_ip\":\"" + wallboxBLE.chargerNetworkIp() + "\"";
    json += ",\"chg_net_signal\":" + String(wallboxBLE.chargerNetworkSignal());
    json += ",\"chg_fw_changed\":" + String(wallboxBLE.firmwareChanged() ? "true" : "false");
    json += ",\"chg_fw_prev\":\"" + wallboxBLE.previousFirmware() + "\"";
    json += ",\"ble_paused\":" + String(wallboxBLE.isPaused() ? "true" : "false");
    json += ",\"ble_pause_remaining\":" + String(wallboxBLE.pauseRemainingMs() / 1000);
    json += ",\"ble_last_activity_s\":" + String(wallboxBLE.lastActivityAge() / 1000);
    json += ",\"auth_enabled\":" + String(configMgr.get().authEnabled && configMgr.get().authPass.length() > 0 ? "true" : "false");
    json += ",\"sta_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += "}";
    http.send(200, "application/json", json);
}

static void handleBlePause() {
    if (!checkAuth()) return;
    uint32_t ms = 5 * 60 * 1000;  // default 5 min
    if (http.hasArg("ms")) ms = (uint32_t) http.arg("ms").toInt();
    if (ms < 30000) ms = 30000;        // min 30s
    if (ms > 30 * 60 * 1000) ms = 30 * 60 * 1000;  // max 30 min
    wallboxBLE.pause(ms);
    http.send(200, "application/json", String("{\"paused_for_s\":") + String(ms / 1000) + "}");
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

// Server-side limit on how many BAPI command requests can be queued.
// Each /api/command call goes through sendCommand which waits on
// _cmdMutex with a multi-second timeout. Under load (e.g. /sessions
// firing 20+ parallel r_log fetches), the queue piles up, LWIP/heap
// gets stressed, and the gateway panics. Capping in-flight at 2 and
// fast-failing the rest with 503 keeps the gateway alive — the
// browser retry on 503 (or the JS-level fetch limiter) keeps the
// experience usable.
static volatile int _apiCmdInflight = 0;
static const int MAX_API_CMD_INFLIGHT = 2;

// Re-entrancy tripwire for WBWebServer::loop(). Normally 0→1→0 per request;
// if g_webMaxReentry ever latches >1, something pumped http.handleClient()
// from inside a handler (the reentrancy class of bug — see onYield in
// main.cpp). Surfaced on /api/health so we can prove over Wi-Fi, with no
// serial console, that it stays at 1. Single-threaded handler → plain ints.
volatile int g_webReentryDepth = 0;
volatile int g_webMaxReentry   = 0;
// Main-loop iteration gap tracker. Updated from main.cpp loop() — the
// difference between consecutive entries tells us if the main task is
// being blocked for long stretches. Any iteration > ~500 ms is
// suspicious; a wedge (per peter-mcc #4 and the maintainer's HAR
// capture) would show as a value in the multi-second range. The
// counter latches at the worst gap observed since boot — cheap to
// read, gives us hard evidence of CPU starvation without needing
// serial console access.
volatile uint32_t g_loopLastMs = 0;
volatile uint32_t g_loopMaxMs  = 0;

// Token bucket on the serialized BLE resource. /api/command ultimately issues
// one BLE round-trip (~500 ms) behind _cmdMutex; this caps the *rate* so a
// pathological client (tight curl loop, HA re-sync storm) gets a clean
// 429 + Retry-After instead of piling up. Sizing: BAPI completes ~1 op / 500 ms,
// so refill 2 tokens/sec; capacity 4 absorbs a legitimate burst (cold page
// load, a couple of Saves). millis()-based lazy refill — no timer, ~0 heap.
// This is rate; MAX_API_CMD_INFLIGHT is concurrency — orthogonal guards.
static const float TB_CAP    = 4.0f;   // burst capacity (tokens)
static const float TB_REFILL = 2.0f;   // tokens per second
static float       _tbTokens = TB_CAP;
static uint32_t    _tbLastMs = 0;

static bool tbAllow() {
    uint32_t now = millis();
    if (_tbLastMs == 0) _tbLastMs = now;
    _tbTokens += (now - _tbLastMs) * (TB_REFILL / 1000.0f);
    if (_tbTokens > TB_CAP) _tbTokens = TB_CAP;
    _tbLastMs = now;
    if (_tbTokens < 1.0f) return false;
    _tbTokens -= 1.0f;
    return true;
}

// Read-only token bucket peek, for surfacing on /api/health and over MQTT
// for HA-observable rate-limit pressure. Refresh-then-read without
// consuming — so a single curl /api/health doesn't perturb the bucket.
int wb_web_tokens_remaining() {
    uint32_t now = millis();
    if (_tbLastMs == 0) _tbLastMs = now;
    _tbTokens += (now - _tbLastMs) * (TB_REFILL / 1000.0f);
    if (_tbTokens > TB_CAP) _tbTokens = TB_CAP;
    _tbLastMs = now;
    return (int)_tbTokens;
}

static void handleApiCommand() {
    if (!checkAuth()) return;
    if (!wallboxBLE.isConnected()) {
        http.send(503, "application/json", "{\"error\":\"BLE not connected\"}");
        return;
    }
    // Rate limit before touching the inflight counter so the bucket gates the
    // true admission point. 429 = slow down (rate); distinct from 503 = busy
    // (concurrency) below, so the two failure modes stay diagnosable.
    if (!tbAllow()) {
        http.sendHeader("Retry-After", "1");
        http.send(429, "application/json", "{\"error\":\"rate_limited\",\"retry\":true}");
        return;
    }
    if (_apiCmdInflight >= MAX_API_CMD_INFLIGHT) {
        http.sendHeader("Retry-After", "1");
        http.send(503, "application/json", "{\"error\":\"busy\",\"retry\":true}");
        return;
    }
    _apiCmdInflight++;
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
        _apiCmdInflight--;
        http.send(400, "application/json", "{\"error\":\"unknown action\"}");
        return;
    }
    _apiCmdInflight--;
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
<div id='auth-warn' style='display:none;background:rgba(245,158,11,.08);border:1px solid rgba(245,158,11,.2);border-radius:8px;padding:10px;margin-bottom:10px;font-size:.82em;color:#f59e0b'>&#x26A0; No web password set — anyone on your WiFi can control this charger. <a href='/config' style='color:#f59e0b;text-decoration:underline'>Set one</a></div>
<div id='pin-warn' style='display:none;background:rgba(245,158,11,.08);border:1px solid rgba(245,158,11,.2);border-radius:8px;padding:10px;margin-bottom:10px;font-size:.82em;color:#f59e0b'>&#x26A0; No Bluetooth Passcode set on the charger — anyone nearby could pair to it. Set one in the Wallbox app, then copy it into the gateway's <a href='/config' style='color:#f59e0b;text-decoration:underline'>config page</a>.</div>
<div id='ble-health' style='display:none;border-radius:8px;padding:10px;margin-bottom:10px;font-size:.82em'></div>
<div id='notif-bar' style='display:none;background:rgba(239,68,68,.08);border:1px solid rgba(239,68,68,.25);border-radius:8px;padding:10px;margin-bottom:10px;font-size:.82em;color:#ef4444;cursor:pointer' onclick='showNotifs()'>&#x1F514; <span id='notif-count'></span> charger notification(s) — tap to view</div>
<div id='notif-modal' style='display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.55);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);z-index:300;align-items:center;justify-content:center;padding:16px' onclick='if(event.target===this)this.style.display=\"none\"'><div id='notif-modal-inner' style='background:var(--card);border-radius:12px;max-width:480px;width:100%;max-height:80vh;overflow:auto;padding:16px'></div></div>

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
)HTML";
    // Status-code map depends on charger model — Plus uses 0-18 enum (per
    // jagheterfredrik/wallbox-ble); MAX uses sparse hardware codes.
    // Plus-family includes Copper SB, Quasar, Quasar 2 (all share the protocol).
    if (configMgr.isPlusFamily()) {
        page += "var SN={0:'Ready',1:'Charging',2:'Waiting for Car',3:'Waiting for Schedule',4:'Paused',5:'Schedule End',6:'Locked',7:'Error',8:'Waiting for Current',9:'Power Sharing (Unconfigured)',10:'Queue (Power Boost)',11:'Discharging',12:'Waiting for MID Auth',13:'MID Safety Margin',14:'OCPP Unavailable',15:'OCPP Finishing',16:'OCPP Reserved',17:'Updating',18:'Queue (Eco Smart)'};\n";
    } else {
        page += "var SN={0:'Disconnected',1:'Connected',2:'Charging',3:'Paused',4:'Scheduled',5:'Discharging',6:'Error',7:'Disconnected',8:'Locked',9:'Updating',10:'Queue (Power)',13:'Waiting for Car',14:'Error',16:'Ready',17:'Connected',18:'Waiting for Schedule',19:'Scheduled',20:'Charging',21:'Charge Complete',22:'Paused by User',23:'Queue (Power Share)',24:'Queue (Eco Smart)',25:'Waiting for Schedule',26:'Discharging',161:'Ready',178:'Paused',179:'Charging',180:'Scheduled',189:'Ready',193:'Paused',194:'Locked',209:'Reserved (OCPP)',210:'Updating'};\n";
    }
    page += R"HTML(
var CHARGER_TZ='UTC';try{CHARGER_TZ=Intl.DateTimeFormat().resolvedOptions().timeZone||'UTC'}catch(e){}
fetch('/api/command?action=bapi&met=g_tzn&par=null',{signal:AbortSignal.timeout(8000)}).then(function(r){return r.json()}).then(function(d){if(d.r&&d.r.timezone)CHARGER_TZ=d.r.timezone}).catch(function(){});
function _setText(id,txt){var el=document.getElementById(id);if(el)el.textContent=txt}
function _setNum(id,val,suffix,fmt){if(typeof val!=='number'||isNaN(val))return;var el=document.getElementById(id);if(!el)return;el.textContent=(fmt?fmt(val):val)+(suffix||'')}
function applyStatusData(s,rt){if(!s||typeof s!=='object')return;if(typeof s.st==='number'){var n=SN[s.st];if(!n&&rt&&typeof rt.charger_status==='number')n=SN[rt.charger_status];_setText('v-st',n||'Code '+s.st)}_setNum('v-pw',s.cp,' kW',function(v){return v.toFixed(2)});var threePhase=(s.L2>0||s.L3>0||(rt&&rt.phases_connection>=2));if(typeof s.L1==='number'){var l1=(s.L1/10).toFixed(1);if(threePhase&&typeof s.L2==='number'&&typeof s.L3==='number'){_setText('l-cr','L1 / L2 / L3');_setText('v-cr',l1+' / '+(s.L2/10).toFixed(1)+' / '+(s.L3/10).toFixed(1)+' A')}else{_setText('l-cr','Charging Current');_setText('v-cr',l1+' A')}}_setNum('v-en',s.en,' kWh',function(v){return (v/100).toFixed(2)});if(typeof s.cur==='number'){_setText('v-mc',s.cur+' A');var sl=document.getElementById('sl');if(sl)sl.value=s.cur;_setText('sv',s.cur+'A')}try{localStorage.setItem('wb-last-status',JSON.stringify({s:s,rt:rt,t:Date.now()}))}catch(e){}if(rt&&typeof rt==='object'){if(typeof rt.lock_status==='number')_setText('v-lk',rt.lock_status==0?'Unlocked':'Locked');if(typeof rt.ocpp_status==='number'){var os={0:'Not Available',1:'Not Configured',2:'Connected',3:'Charging'};_setText('v-oc',os[rt.ocpp_status]||'Code '+rt.ocpp_status)}}window._lastUpdate=Date.now()}
function applyMeterData(d){if(!d||typeof d!=='object')return;if(typeof d.v1==='number'){var vt=document.getElementById('v-vt');if(vt)vt.textContent=d.v1+' V'}if(typeof d.p1==='number'){var gp=document.getElementById('v-gp');if(gp)gp.textContent=d.p1+' W'}try{localStorage.setItem('wb-last-meter',JSON.stringify({d:d,t:Date.now()}))}catch(e){}}
function P(){if(window.wbws&&window.wbws.isOpen())return;fetch('/api/charger').then(function(r){return r.json()}).then(function(d){if(!d.status||d.status==='null')return;var s=d.status?d.status.r:null,rt=d.realtime?d.realtime.r:null;applyStatusData(s,rt)}).catch(function(){});fetch('/api/command?action=bapi&met=r_dca&par=null').then(function(r){return r.json()}).then(function(d){applyMeterData(d.r)}).catch(function(){})}
// Hook WS push handlers
if(window.wbws){window.wbws.subscribe('status',function(d){var s=d&&d.r?d.r:d;applyStatusData(s,null);if(window.wbCheckStatus)window.wbCheckStatus(s)});window.wbws.subscribe('meter',function(d){applyMeterData(d&&d.r?d.r:d)});}
// Render cached values immediately (no spinners)
try{var c=JSON.parse(localStorage.getItem('wb-last-status')||'null');if(c)applyStatusData(c.s,c.rt)}catch(e){}
try{var cm=JSON.parse(localStorage.getItem('wb-last-meter')||'null');if(cm)applyMeterData(cm.d)}catch(e){}
function C(a){toast('Sending '+a.split('&')[0]+'...','info');fetch('/api/command?action='+a).then(function(x){return x.json()}).then(function(d){if(d.error)toast(d.error,'error');else toast('Command sent','success');setTimeout(P,1000)}).catch(function(e){toast('Error: '+e,'error')})}
var _curTimer=null;
function setCurrent(v){if(_curTimer)clearTimeout(_curTimer);_curTimer=setTimeout(function(){toast('Setting '+v+'A','info');fetch('/api/command?action=current&value='+v).then(function(x){return x.json()}).then(function(d){if(d.error)toast(d.error,'error');else toast('Max current set to '+v+'A','success');setTimeout(P,1000)}).catch(function(e){toast('Error: '+e,'error')})},300)}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
P();setInterval(P,10000);
fetch('/api/command?action=bapi&met=read_pin&par=null',{signal:AbortSignal.timeout(8000)}).then(function(r){return r.json()}).then(function(d){if(d.r&&!d.r.pin)document.getElementById('pin-warn').style.display='block'}).catch(function(){});
fetch('/api/status',{signal:AbortSignal.timeout(4000)}).then(function(r){return r.json()}).then(function(s){if(s.sta_connected&&!s.auth_enabled){var b=document.getElementById('auth-warn');if(b)b.style.display='block'}}).catch(function(){});
var _notifs=[];
function loadNotifs(){fetch('/api/status',{signal:AbortSignal.timeout(4000)}).then(function(r){return r.json()}).then(function(s){if(s.ble!=='connected')return;return fetch('/api/command?action=bapi&met=r_not&par=null',{signal:AbortSignal.timeout(10000)}).then(function(r){return r.json()}).then(function(d){var v=d.r;if(!Array.isArray(v))return;_notifs=v;var bar=document.getElementById('notif-bar');if(!bar)return;if(v.length>0){document.getElementById('notif-count').textContent=v.length;bar.style.display='block'}else{bar.style.display='none'}})}).catch(function(){})}
function showNotifs(){var m=document.getElementById('notif-modal');var inner=document.getElementById('notif-modal-inner');var html="<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px'><h3 style='margin:0'>&#x1F514; Notifications</h3><button onclick='document.getElementById(\"notif-modal\").style.display=\"none\"' style='background:transparent;border:none;color:var(--text2);font-size:1.6em;cursor:pointer;line-height:1'>×</button></div>";if(!_notifs.length){html+="<div style='color:var(--text3)'>No notifications</div>"}else{_notifs.forEach(function(n,i){var msg=n.message||n.msg||n.text||JSON.stringify(n);var ts=(n.timestamp||n.ts)?new Date((n.timestamp||n.ts)*1000).toLocaleString(undefined,{timeZone:CHARGER_TZ}):'';html+="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:6px 0'><div style='font-weight:500;font-size:.9em'>#"+(i+1)+" "+msg+"</div>"+(ts?"<div style='font-size:.78em;color:var(--text3);margin-top:4px'>"+ts+"</div>":'')+"</div>"})}inner.innerHTML=html;m.style.display='flex'}
function updateBleHealth(){fetch('/api/status',{signal:AbortSignal.timeout(5000)}).then(function(r){return r.json()}).then(function(s){var bar=document.getElementById('ble-health');if(!bar)return;var st=s.ble,rssi=s.rssi,age=s.ble_last_activity_s||0;var html='',bg='',bd='',col='';if(st!=='connected'){bg='rgba(239,68,68,.08)';bd='rgba(239,68,68,.3)';col='#ef4444';html='&#x26A0; BLE '+(st||'disconnected')+' — gateway can’t reach the charger. Try moving the ESP32 closer.'}else if(rssi<-90){bg='rgba(239,68,68,.08)';bd='rgba(239,68,68,.3)';col='#ef4444';html='&#x26A0; BLE signal very weak ('+rssi+' dBm) — move the ESP32 closer to the charger for reliable control.'}else if(age>120){bg='rgba(239,68,68,.08)';bd='rgba(239,68,68,.3)';col='#ef4444';html='&#x26A0; BLE link unresponsive ('+age+'s since last reply at '+rssi+' dBm) — commands likely failing, move ESP32 closer or power-cycle.'}else if(rssi<-80){bg='rgba(245,158,11,.08)';bd='rgba(245,158,11,.3)';col='#f59e0b';html='&#x26A0; BLE signal weak ('+rssi+' dBm) — commands may be slow. Consider moving the ESP32 closer.'}else if(age>60){bg='rgba(245,158,11,.08)';bd='rgba(245,158,11,.3)';col='#f59e0b';html='&#x26A0; BLE struggling ('+age+'s since last reply, '+rssi+' dBm) — performance degraded.'}else{bar.style.display='none';return}bar.style.background=bg;bar.style.border='1px solid '+bd;bar.style.color=col;bar.innerHTML=html;bar.style.display='block'}).catch(function(){})}
loadNotifs();setInterval(loadNotifs,60000);
updateBleHealth();setInterval(updateBleHealth,15000);
</script>
</div>
)HTML";
    page += htmlFoot("/");
    http.send(200, "text/html", page);
}

// ========== PAGE 2: Settings (/settings) ==========
static void handleSettings() {
    // /settings is the largest page (~67 KB after rc21 additions).
    // Build-then-send with one accumulating String was truncating
    // mid-body around 65 KB on fragmented heap: subsequent += calls
    // dropped silently, the response was missing its htmlFoot, and
    // the body.classList.add('ready') script never ran. User saw a
    // black screen because body{visibility:hidden} was never lifted.
    //
    // Fix: stream each section via sendContent() with chunked
    // encoding. Each temporary String holds only one section, gets
    // freed as soon as the chunk is on the wire, and never has to
    // be reallocated past 65 KB.
    http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    http.send(200, "text/html", "");
    http.sendContent(htmlHead("Settings"));
    http.sendContent(R"HTML(
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
    <div class='card-header'><span class='card-icon'>&#x1F4C5;</span><h2>Charging Schedules</h2></div>
    <div id='sch-timeline-wrap' style='display:none;overflow-x:auto;-webkit-overflow-scrolling:touch;margin:0 -4px 12px;padding:0 4px'>
      <div id='sch-timeline' style='display:grid;grid-template-columns:36px repeat(24,minmax(10px,1fr));gap:1px;font-size:.65em;min-width:320px'></div>
      <div id='sch-legend' style='display:flex;flex-wrap:wrap;gap:6px;margin-top:6px;font-size:.72em'></div>
    </div>
    <div id='sch-list'><span class='spinner'></span> Loading...</div>
    <div class='row' style='margin-top:12px'>
      <button class='btn btn-success' onclick='newSchedule()'>+ Add New</button>
      <a href='/sessions' class='btn btn-outline' style='text-decoration:none;text-align:center'>&#x1F4CA; Heatmap</a>
    </div>
    <div id='qr' style='display:none;margin-top:12px'></div>
  </div>

  <div class='card' id='sched-edit' style='display:none'>
  <div class='card-header'><span class='card-icon'>&#x270E;</span><h2 id='sch-form-title'>New Schedule</h2></div>
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
    <button class='btn btn-success' onclick='saveSch()'>Save</button>
    <button class='btn btn-outline' onclick='cancelEdit()'>Cancel</button>
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
      <button class='btn btn-outline' style='padding:12px' onclick='E("phasesw")'>&#x1F504; Phase Switch</button>
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
      <button class='btn btn-outline' style='padding:12px' onclick='showWiFi()'>&#x1F4F6; WiFi Status</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("halo")'>&#x1F4A1; Halo LED</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("theme")'>&#x1F3A8; Theme</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("cost")'>&#x1F4B0; Charging Cost</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("notif")'>&#x1F514; Notifications</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("pin")'>&#x1F511; Bluetooth Passcode</button>
      <button class='btn btn-outline' style='padding:12px' onclick='E("ota")'>&#x1F4E6; Firmware Update</button>
      <button id='ble-pause-btn' class='btn btn-outline' style='padding:12px' onclick='pauseBle()'>&#x1F4F4; Release BLE for App</button>
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
function utcToLocal(hhmm){var h=parseInt(hhmm.slice(0,2)),m=parseInt(hhmm.slice(2));try{var now=new Date();return new Date(Date.UTC(now.getUTCFullYear(),now.getUTCMonth(),now.getUTCDate(),h,m)).toLocaleTimeString('en-AU',{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){var d=new Date();d.setUTCHours(h,m,0,0);return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)}}
function localToUtc(hhmm){var p=hhmm.split(':');try{var now=new Date();var d=new Date(now.getFullYear(),now.getMonth(),now.getDate(),parseInt(p[0]),parseInt(p[1]));var utc=new Date(d.toLocaleString('en-US',{timeZone:'UTC'}));var local=new Date(d.toLocaleString('en-US',{timeZone:CHARGER_TZ}));var diff=local-utc;var ud=new Date(d.getTime()-diff);return ('0'+ud.getUTCHours()).slice(-2)+('0'+ud.getUTCMinutes()).slice(-2)}catch(e){var d2=new Date();d2.setHours(parseInt(p[0]),parseInt(p[1]),0,0);return ('0'+d2.getUTCHours()).slice(-2)+('0'+d2.getUTCMinutes()).slice(-2)}}
function Q(m,l){var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;if(!r)r=document.getElementById('qr');if(!r){toast('No result panel found','error');return}r.style.display='block';r.innerHTML="<span class='spinner'></span>"+l+"...";var doFetch=function(){fetch('/api/command?action=bapi&met='+m+'&par=null',{signal:AbortSignal.timeout(15000)}).then(function(x){return x.json()}).then(function(d){if(d.error){r.innerHTML='<span style="color:var(--danger)">'+d.error+'</span>';return}r.innerHTML=F(m,l,d.r||d)}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'})};if(m==='r_schs'&&tzReady){tzReady.then(doFetch)}else{doFetch()}}
function F(m,l,r){var h="<div style='font-weight:600;color:var(--accent);margin-bottom:6px'>"+l+"</div>";if(m==='gwsta'){var s={0:'Disconnected',1:'Connected',2:'Connecting'};h+=row('WiFi',s[r]||'Code '+r)}else if(m==='r_ses'){h+=row('Last Session',r.last);h+=row('Total Sessions',r.size)}else if(m==='r_schs'){var sc=r.schedules||r;if(!Array.isArray(sc)){h+=row('Schedules','None');return h}sc.forEach(function(s,i){var ds='';for(var b=0;b<7;b++)if(s.days&(1<<b))ds+=DAYS[b]+' ';h+="<div style='background:var(--bg);border-radius:8px;padding:8px;margin:4px 0'><div style='display:flex;justify-content:space-between'><b>Schedule "+(i+1)+"</b><span class='badge "+(s.enabled?'badge-success':'badge-warning')+"'>"+(s.enabled?'Active':'Off')+"</span></div>";h+=row('Time',utcToLocal(s.start)+' - '+utcToLocal(s.stop));h+=row('Days',ds.trim()||'None');h+=row('Power Limit',(s.mcr<=1||s.mcr>=32)?'No limit':s.mcr+' A');if(s.target&&s.target.type==1)h+=row('Energy Limit',(s.target.value/1000)+' kWh');else h+=row('Energy Limit','No limit');h+="</div>"});if(!sc.length)h+=row('Schedules','None')}else if(m==='r_dca'){if(typeof r==='object'){h+=row('Voltage L1',r.v1+' V');if(r.v2)h+=row('Voltage L2',r.v2+' V');if(r.v3)h+=row('Voltage L3',r.v3+' V');h+=row('Power L1',r.p1+' W');if(r.p2)h+=row('Power L2',r.p2+' W');if(r.p3)h+=row('Power L3',r.p3+' W');h+=row('Current L1',(r.c1/10).toFixed(1)+' A');if(r.c2)h+=row('Current L2',(r.c2/10).toFixed(1)+' A');if(r.c3)h+=row('Current L3',(r.c3/10).toFixed(1)+' A');h+=row('Total Energy',(r.e/1000).toFixed(1)+' kWh')}else{h+=row('Energy Meter',''+r)}}else if(m==='g_alo'){if(typeof r==='object'){h+=row('Auto Lock',r.enabled?'Enabled':'Disabled');if(r.time)h+=row('Lock After',r.time+' seconds')}else{h+=row('Auto Lock',r?'Enabled':'Disabled')}}else if(m==='g_ecos'){var em={0:'Disabled',1:'Full Green (Solar Only)',2:'Eco Smart (Solar + Grid)'};if(typeof r==='object'){h+=row('Status',r.ese?'Active':'Inactive');h+=row('Mode',em[r.esm]||'Mode '+r.esm);h+=row('Solar Power Target',r.esp+'%')}else{h+=row('Eco Smart',em[r]||''+r)}}else if(m==='g_phsw'){if(typeof r==='object'){h+=row('Phase Switch',r.enabled?'Enabled':'Disabled')}else{h+=row('Phase Switch',r?'Enabled':'Disabled')}}else if(m==='read_pin'){if(typeof r==='object'){h+=row('BLE PIN',r.pin||'Not set');h+=row('PIN Version',r.version||'None')}else{h+=row('BLE PIN',''+r)}}else if(m==='r_hsh'){h+=row('ICP Max Current',r+'A')}else if(m==='g_psh'){if(typeof r==='object'){h+=row('Dynamic Power Sharing',r.dyps?'Enabled':'Disabled');h+=row('Max Power Per Charger',r.mcpp?r.mcpp+'W':'Unlimited');h+=row('Min Current',r.minI+'A');h+=row('Chargers in Group',r.nchg)}else{var ps={0:'Disabled',1:'Enabled',2:'Active'};h+=row('Power Sharing',ps[r]||''+r)}}else if(m==='g_tzn'){h+=row('Timezone',r.timezone||r)}else{h+="<pre style='margin:0;white-space:pre-wrap;font-size:.82em'>"+JSON.stringify(r,null,2)+"</pre>"}return h}
function showTimezone(){E('tz')}
function saveTz(){var tz=document.getElementById('tz-select').value;var p=JSON.stringify({timezone:tz});toast('Saving timezone...','info');fetch('/api/command?action=bapi&met=s_tzn&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){if(d.error){toast(d.error,'error')}else{CHARGER_TZ=tz;toast('Timezone set to '+tz,'success')}}).catch(function(e){toast('Error: '+e.message,'error')})}
function E(type){var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;if(!r)return;r.style.display='block';var h='';if(type==='autolock'){h="<h2>&#x1F510; Auto Lock</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_alo&par=null',{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){
/* The Pulsar MAX returns r as a plain number (0=disabled, non-zero=enabled
   minutes-until-lock). Newer firmware returns {enabled, time}. Handle both. */
var en,tm,simple=false;
if(d.r&&typeof d.r==='object'){en=d.r.enabled?true:false;tm=(typeof d.r.time==='number')?d.r.time:60;}
else if(typeof d.r==='number'){simple=true;en=d.r>0;tm=d.r>0?d.r:60;}
else{r.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn’t read Auto Lock state. <button class="btn btn-outline" style="padding:4px 10px;margin-top:6px" onclick=\'E("autolock")\'>Retry</button></div>';return}
r.innerHTML="<h2>&#x1F510; Auto Lock</h2>"+row('Current state',en?'Enabled':'Disabled')+(simple?"<p class='help'>Older firmware: a single value is sent (minutes; 0 = off).</p>":"")+"<label style='margin-top:12px'>Enabled</label><select id='al-en'><option value='0'"+(en?'':' selected')+">Disabled</option><option value='1'"+(en?' selected':'')+">Enabled</option></select><label>Lock After "+(simple?'(minutes)':'(seconds)')+"</label><input type='number' id='al-time' value='"+tm+"' min='1' max='600'><input type='hidden' id='al-simple' value='"+(simple?'1':'0')+"'><div class='row' style='margin-top:10px'><button class='btn btn-success' onclick='saveAutoLock()'>Save</button></div><div id='al-result' style='display:none;margin-top:10px'></div>"
}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='ocpp'){h="<h2>&#x1F517; OCPP</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_ocpp&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){if(!d.r||typeof d.r!=='object'){r.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn\u2019t read OCPP state. <button class=\'btn btn-outline\' style=\'padding:4px 10px;margin-top:6px\' onclick=\'E("ocpp")\'>Retry</button></div>';return}var o=d.r;r.innerHTML="<h2>&#x1F517; OCPP Configuration</h2><label>Server URL</label><input id='ocpp-url' value='"+(o.u||'')+"' placeholder='ws://server:9000'><div class='row'><div><label>Charger ID</label><input id='ocpp-id' value='"+(o.chid||'')+"'></div><div><label>Password</label><input id='ocpp-pw' type='password' value='"+(o.pw||'')+"'></div></div><label>Enabled</label><select id='ocpp-en'><option value='0'"+(o.e?'':' selected')+">Disabled</option><option value='1'"+(o.e?' selected':'')+">Enabled</option></select><button class='btn btn-success' style='margin-top:10px' onclick='saveOcpp()'>Save OCPP</button><div id='ocpp-result' style='display:none;margin-top:10px'></div>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='eco'){h="<h2>&#x2600; Eco Smart</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_ecos&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){if(!d.r||typeof d.r!=='object'){r.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn\u2019t read current Eco Smart state. <button class=\'btn btn-outline\' style=\'padding:4px 10px;margin-top:6px\' onclick=\'E("eco")\'>Retry</button></div>';return}var o=d.r;var esm=(typeof o.esm==='number')?o.esm:0;var esp=(typeof o.esp==='number')?o.esp:50;var ese=o.ese?true:false;var modes=[{v:0,t:'Disabled'},{v:1,t:'Full Green (Solar Only)'},{v:2,t:'Eco Smart (Solar + Grid)'}];var opts='';modes.forEach(function(m){opts+="<option value='"+m.v+"'"+(m.v===esm?' selected':'')+">"+m.t+"</option>"});r.innerHTML="<h2>&#x2600; Eco Smart</h2>"+row('Current state',ese?'Active':'Inactive')+"<label style='margin-top:12px'>Mode</label><select id='eco-mode'>"+opts+"</select><label>Solar Power Target (%)</label><input type='number' id='eco-esp' value='"+esp+"' min='0' max='100'><p class='help'>Eco Smart uses solar surplus. Full Green only charges from solar.</p><button class='btn btn-success' style='margin-top:10px' onclick='saveEco()'>Save</button><div id='eco-result' style='display:none;margin-top:10px'></div>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='tz'){h="<h2>&#x1F30D; Timezone</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_tzn&par=null',{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){if(!d.r||typeof d.r!=='object'||!d.r.timezone){r.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn\u2019t read timezone. <button class=\'btn btn-outline\' style=\'padding:4px 10px;margin-top:6px\' onclick=\'E("tz")\'>Retry</button></div>';return}var tz=d.r.timezone;CHARGER_TZ=tz;var opts='';var zones=['Australia/Sydney','Australia/Melbourne','Australia/Brisbane','Australia/Adelaide','Australia/Perth','Australia/Darwin','Australia/Hobart','Asia/Tokyo','Asia/Shanghai','Asia/Singapore','Asia/Kolkata','Asia/Dubai','Europe/London','Europe/Paris','Europe/Berlin','America/New_York','America/Chicago','America/Los_Angeles','America/Toronto','Pacific/Auckland','UTC'];zones.forEach(function(z){opts+="<option value='"+z+"'"+(z===tz?' selected':'')+">"+z.replace('_',' ')+"</option>"});r.innerHTML="<h2>&#x1F30D; Timezone</h2>"+row('Current',tz)+"<label style='margin-top:12px'>Change To</label><select id='tz-select'>"+opts+"</select><button class='btn btn-success' style='margin-top:10px' onclick='saveTz()'>Save</button><div id='tz-result' style='display:none;margin-top:10px'></div>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='phasesw'){h="<h2>&#x1F504; Phase Switch</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_phsw&par=null',{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){if(!d.r||typeof d.r!=='object'){r.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn\u2019t read Phase Switch state. <button class=\'btn btn-outline\' style=\'padding:4px 10px;margin-top:6px\' onclick=\'E("phasesw")\'>Retry</button></div>';return}var en=d.r.enabled?true:false;r.innerHTML="<h2>&#x1F504; Phase Switch</h2>"+row('Current state',en?'Enabled':'Disabled')+"<label style='margin-top:12px'>Enabled</label><select id='phsw-en'><option value='0'"+(en?'':' selected')+">Disabled</option><option value='1'"+(en?' selected':'')+">Enabled</option></select><p class='help'>Auto-switches between 1 and 3 phase based on solar surplus.</p><button class='btn btn-success' style='margin-top:10px' onclick='savePhaseSw()'>Save</button>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='halo'){h="<h2>&#x1F4A1; Halo LED</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";r.innerHTML=h;fetch('/api/command?action=bapi&met=g_halocfg&par=null',{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).then(function(d){if(!d.r||typeof d.r!=='object'){r.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn\u2019t read Halo state. <button class=\'btn btn-outline\' style=\'padding:4px 10px;margin-top:6px\' onclick=\'E("halo")\'>Retry</button></div>';return}var o=d.r;var bright=(typeof o.bright==='number')?o.bright:100;var mode=(typeof o.mode==='number')?o.mode:1;var ts=(typeof o.time_s==='number')?o.time_s:10;r.innerHTML="<h2>&#x1F4A1; Halo LED</h2>"+row('Current',(mode?'Standby on':'Standby off')+' \u00B7 '+bright+'%')+"<label style='margin-top:12px'>Standby</label><select id='halo-m'><option value='1'"+(mode===1?' selected':'')+">On (dim when idle)</option><option value='0'"+(mode===0?' selected':'')+">Off (always bright)</option></select><label>Brightness (%)</label><input type='range' id='halo-b' min='0' max='100' value='"+bright+"' oninput=\"document.getElementById('halo-bv').textContent=this.value+'%'\"><div id='halo-bv' style='text-align:center;margin-top:-4px;color:var(--text3);font-size:.85em'>"+bright+"%</div><label>Standby Timeout (seconds)</label><input type='number' id='halo-t' value='"+ts+"' min='0' max='3600'><p class='help'>How long to wait before dimming when standby is on.</p><button class='btn btn-success' style='margin-top:10px' onclick='saveHalo()'>Save</button>"}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'});return}else if(type==='pin'){
  /* Bluetooth Passcode — two independent facts surfaced here:
       1. Gateway-stored value (cfg.blePin, masked via /api/status):
          what WE send when pairing.
       2. Charger-expected value (read_pin BAPI): what the CHARGER
          requires on the next pair attempt.
     They should match. When they don't, the user sees that here and
     knows which side to fix. Our save updates only side (1) — the
     charger-side passcode is created in the official Wallbox app. */
  r.innerHTML="<h2>&#x1F511; Bluetooth Passcode</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";
  Promise.all([
    fetch('/api/status').then(function(x){return x.json()}).catch(function(){return{}}),
    fetch('/api/command?action=bapi&met=read_pin&par=null',{signal:AbortSignal.timeout(10000)}).then(function(x){return x.json()}).catch(function(){return{}})
  ]).then(function(results){
    var s=results[0]||{};
    var bapi=results[1]||{};
    var gwHas=s.ble_pin==='***';
    var chgPin=(bapi.r&&typeof bapi.r==='object')?bapi.r.pin:undefined;
    var chgHas=(chgPin!==null&&chgPin!==undefined&&chgPin!=='');
    var chgUnknown=(bapi.r===undefined||(typeof bapi.r==='object'&&!('pin' in bapi.r))||bapi.error);
    var gwTxt=gwHas?"<span style='color:#22c55e'>&#x2713; Configured</span>":"<span style='color:#f59e0b'>&#x26A0; Not set</span>";
    var chgTxt=chgUnknown?"<span style='color:var(--text3)'>Unknown (charger didn’t answer)</span>":chgHas?"<span style='color:#22c55e'>&#x2713; Required (charger expects a passcode)</span>":"<span style='color:#94a3b8'>Not required</span>";
    var mismatch='';
    if(!chgUnknown){
      if(gwHas&&!chgHas)mismatch="<div style='background:rgba(245,158,11,.08);border:1px solid rgba(245,158,11,.3);border-radius:8px;padding:10px;margin:8px 0;font-size:.85em;color:#f59e0b'>&#x26A0; Gateway has a stored passcode but the charger no longer requires one. Save with the field blank to clear.</div>";
      else if(!gwHas&&chgHas)mismatch="<div style='background:rgba(239,68,68,.08);border:1px solid rgba(239,68,68,.3);border-radius:8px;padding:10px;margin:8px 0;font-size:.85em;color:#ef4444'>&#x26A0; Charger requires a passcode but the gateway has none stored. BLE pair will fail until you enter it below.</div>";
    }
    r.innerHTML="<h2>&#x1F511; Bluetooth Passcode</h2>"+row('Gateway has stored',gwTxt)+row('Charger expects',chgTxt)+mismatch+"<label style='margin-top:12px'>New Passcode</label><input type='text' id='pin-new' value='' placeholder='8 digits from the Wallbox app' maxlength='16' autocomplete='off' inputmode='numeric'><p class='help'>Open the Wallbox app \xE2\x86\x92 your charger \xE2\x86\x92 Settings \xE2\x86\x92 <b>Bluetooth Passcode</b>. Copy the 8-digit code here. Leave blank to clear the gateway’s stored value. Saving reboots the gateway so the new value takes effect on the next pair.</p><button class='btn btn-success' style='margin-top:10px' onclick='savePin()'>Save &amp; Reboot</button><div id='pin-result' style='display:none;margin-top:10px'></div>";
  }).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'});
  return
}else if(type==='ota'){location.href='/ota';return
}else if(type==='theme'){var cur='auto';try{cur=localStorage.getItem('wb-theme')||'auto'}catch(e){}var opts='';[{v:'auto',t:'Auto (follow system)'},{v:'dark',t:'Dark'},{v:'light',t:'Light'}].forEach(function(o){opts+="<option value='"+o.v+"'"+(o.v===cur?' selected':'')+">"+o.t+"</option>"});r.innerHTML="<h2>&#x1F3A8; Theme</h2><label>Appearance</label><select id='theme-sel' onchange='applyTheme(this.value)'>"+opts+"</select><p class='help'>Auto follows your OS or browser preference.</p>";return}else if(type==='cost'){
  var T;try{T=JSON.parse(localStorage.getItem('wb-tariff')||'null')}catch(e){T=null}
  if(!T)T={enabled:false,currency:'$',baseRate:0.30,greenRate:0,tiers:[]};
  window._editTariff=T;
  r.innerHTML=renderCostPanel(T);
  return
}else if(type==='notif'){
  var supported=(typeof Notification!=='undefined');
  var secure=(window.isSecureContext===true);
  var perm=supported?Notification.permission:'unsupported';
  var canUseLocal=supported&&secure&&perm!=='denied';
  var h2="<h2>&#x1F514; Notifications</h2>";
  if(!secure){
    h2+="<div style='background:rgba(245,158,11,.08);border:1px solid rgba(245,158,11,.25);border-radius:8px;padding:10px;margin-bottom:12px;font-size:.85em;color:#f59e0b'>&#x26A0; Your browser is blocking notifications because this page is served over plain HTTP. Browsers require HTTPS for the Notification API. Use the Home Assistant route below instead.</div>";
  }
  h2+="<h3 style='font-size:.95em;margin:12px 0 8px;color:var(--text2)'>Recommended: Home Assistant push</h3>";
  h2+="<div style='font-size:.82em;color:var(--text3);margin-bottom:8px'>HA's Companion app already gives you proper phone notifications. Add this automation in HA (Settings &rarr; Automations &rarr; YAML mode):</div>";
  h2+="<pre style='background:var(--bg);border-radius:8px;padding:10px;font-size:.72em;overflow-x:auto;margin:0;color:var(--text)'>"+"alias: Wallbox charging complete\\ntrigger:\\n  - platform: state\\n    entity_id: sensor.wallbox_pulsar_max_status\\n    to: Charge Complete\\naction:\\n  - service: notify.mobile_app_YOUR_PHONE\\n    data:\\n      title: Charging complete\\n      message: Car is ready ({{ states('sensor.wallbox_pulsar_max_session_energy') }} kWh)".replace(/\\\\n/g,'\\n')+"</pre>";
  h2+="<div style='font-size:.78em;color:var(--text3);margin-top:6px'>Replace <code>YOUR_PHONE</code> with your Companion app's mobile_app entity. Repeat the trigger block with different <code>to:</code> values (<code>Error</code>, <code>Charging</code>) for other events.</div>";
  if(canUseLocal){
    var N=loadNotifSettings();
    h2+="<h3 style='font-size:.95em;margin:18px 0 8px;color:var(--text2)'>Alternative: browser notifications (foreground only)</h3>";
    h2+="<div style='font-size:.82em;color:var(--text3);margin-bottom:8px'>Permission status: "+(perm==='granted'?'<span style=\"color:#22c55e\">Granted</span>':'Not yet asked')+". Notifications only fire while this site is open in a tab.</div>";
    h2+="<label style='display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:6px'><input type='checkbox' id='notif-en' "+(N.enabled?'checked':'')+" style='width:auto'> Enable browser notifications</label>";
    var events=[{k:'started',t:'Charging starts'},{k:'complete',t:'Charging complete'},{k:'paused',t:'Charging paused'},{k:'error',t:'Charger error'},{k:'plug_in',t:'Plug connected'},{k:'plug_out',t:'Plug disconnected'}];
    h2+="<div style='font-size:.82em;color:var(--text2);margin-top:8px'>Trigger on:</div>";
    events.forEach(function(e){h2+="<label style='display:flex;align-items:center;gap:8px;cursor:pointer;margin-top:6px'><input type='checkbox' data-notif='"+e.k+"' "+(N.events[e.k]?'checked':'')+" style='width:auto'>"+e.t+"</label>"});
    h2+="<div class='row' style='margin-top:14px'><button class='btn btn-success' onclick='saveNotif()'>Save</button><button class='btn btn-outline' onclick='testNotif()'>Send test</button></div>";
  }
  r.innerHTML=h2;
  return
}r.innerHTML=h}
function loadNotifSettings(){
  var def={enabled:false,events:{started:true,complete:true,paused:false,error:true,plug_in:false,plug_out:false}};
  try{var s=JSON.parse(localStorage.getItem('wb-notif')||'null');if(!s)return def;if(!s.events)s.events=def.events;return s}catch(e){return def}
}
function saveNotif(){
  var en=document.getElementById('notif-en').checked;
  var events={};
  document.querySelectorAll('[data-notif]').forEach(function(c){events[c.getAttribute('data-notif')]=c.checked});
  var s={enabled:en,events:events};
  if(en&&typeof Notification!=='undefined'&&Notification.permission==='default'){
    Notification.requestPermission().then(function(p){if(p!=='granted'){toast('Notifications were not granted','warning')}});
  }
  try{localStorage.setItem('wb-notif',JSON.stringify(s));toast('Notifications saved','success')}catch(e){toast('Save failed','error')}
}
function testNotif(){
  if(typeof Notification==='undefined'){toast('Browser does not support notifications','error');return}
  if(Notification.permission==='default'){Notification.requestPermission().then(function(p){if(p==='granted')fireNotif('Test','This is a test notification from your Wallbox gateway.')});return}
  if(Notification.permission!=='granted'){toast('Notifications blocked in browser settings','error');return}
  fireNotif('Test','This is a test notification from your Wallbox gateway.');
}
function fireNotif(title,body){
  try{
    var icon='data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="%233b82f6"><path d="M14 3v6h4l-7 12v-9H7l7-9z"/></svg>';
    new Notification(title,{body:body,icon:icon,tag:'wb',silent:false});
  }catch(e){console.error('Notification failed',e)}
}
// Charging-status transition watcher — fires browser notifications on WS pushes
var _lastSt=null;
function watchStatusForNotif(s){
  if(!s||typeof s.st!=='number')return;
  var st=s.st;
  if(_lastSt===null){_lastSt=st;return}
  if(st===_lastSt)return;
  var N=loadNotifSettings();
  if(!N.enabled||typeof Notification==='undefined'||Notification.permission!=='granted'){_lastSt=st;return}
  // Status code groups
  var charging=[2,20,179];
  var complete=[21];
  var paused=[3,22,178,193];
  var error=[6,14];
  var ready=[0,7,16,161,189];
  var connected=[1,13,17];
  var was=_lastSt;
  function inGroup(s,g){return g.indexOf(s)>=0}
  if(inGroup(st,complete)&&N.events.complete)fireNotif('Charging complete','Your car is ready to go.');
  else if(inGroup(st,charging)&&!inGroup(was,charging)&&N.events.started)fireNotif('Charging started',(SN[st]||'Active')+'.');
  else if(inGroup(st,paused)&&!inGroup(was,paused)&&N.events.paused)fireNotif('Charging paused',SN[st]||'Paused.');
  else if(inGroup(st,error)&&!inGroup(was,error)&&N.events.error)fireNotif('Charger error',SN[st]||'See dashboard.');
  else if(inGroup(st,connected)&&inGroup(was,ready)&&N.events.plug_in)fireNotif('Plug connected','Ready to start charging.');
  else if(inGroup(st,ready)&&!inGroup(was,ready)&&N.events.plug_out)fireNotif('Plug disconnected','Charger is now idle.');
  _lastSt=st;
}
function saveAutoLock(){
  /* Older firmware (Pulsar MAX): the charger expects a single number par
     — minutes-until-lock, 0 = off. Newer firmware: full object with
     enabled+time. The form remembers which shape it loaded with so we
     send the right one back. */
  var simple=document.getElementById('al-simple').value==='1';
  var en=parseInt(document.getElementById('al-en').value);
  var t=parseInt(document.getElementById('al-time').value);
  var p=simple?String(en?t:0):JSON.stringify({enabled:en,time:t});
  toast('Saving auto lock...','info');
  fetch('/api/command?action=bapi&met=s_alo&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'Auto lock saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})
}
function saveEco(){var mode=parseInt(document.getElementById('eco-mode').value);var espEl=document.getElementById('eco-esp');var esp=espEl?parseInt(espEl.value)||0:50;var p=JSON.stringify({mode:mode,esp:esp});toast('Saving eco smart...','info');fetch('/api/command?action=bapi&met=s_ecos&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'Eco Smart saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function saveOcpp(){var p=JSON.stringify({u:document.getElementById('ocpp-url').value,chid:document.getElementById('ocpp-id').value,pw:document.getElementById('ocpp-pw').value,e:parseInt(document.getElementById('ocpp-en').value)});toast('Saving OCPP...','info');fetch('/api/command?action=bapi&met=s_ocpp&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'OCPP config saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function savePhaseSw(){var en=parseInt(document.getElementById('phsw-en').value);var p=JSON.stringify({enabled:en});toast('Saving phase switch...','info');fetch('/api/command?action=bapi&met=s_phsw&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'Phase switch saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function saveHalo(){var b=parseInt(document.getElementById('halo-b').value);var m=parseInt(document.getElementById('halo-m').value);var t=parseInt(document.getElementById('halo-t').value)||0;var p=JSON.stringify({bright:b,mode:m,time_s:t});toast('Saving halo...','info');fetch('/api/command?action=bapi&met=s_halocfg&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){toast(d.error||'Halo saved!',d.error?'error':'success')}).catch(function(e){toast('Error: '+e.message,'error')})}
function savePin(){
  /* Local gateway setting — does NOT call s_pin on the charger.
     Updates cfg.blePin in NVS so the next BLE pair uses the new value,
     then reboots. Reboot is required because NimBLE has already
     established a session under the old credentials. */
  var v=document.getElementById('pin-new').value.trim();
  if(v.length>0&&!/^[0-9]+$/.test(v)){toast('Passcode must be digits only','error');return}
  var res=document.getElementById('pin-result');
  toast('Saving passcode...','info');
  fetch('/api/pin?csrf='+window.WB_CSRF+'&pin='+encodeURIComponent(v),{method:'POST'}).then(function(x){return x.json()}).then(function(d){
    if(d.error){toast(d.error,'error');return}
    if(res){res.style.display='block';res.style.color='var(--success)';res.textContent='Saved \xE2\x80\x94 rebooting...'}
    toast('Passcode saved — gateway is rebooting','success');
  }).catch(function(e){toast('Error: '+e.message,'error')})
}
function showWiFi(){
  /* Gateway-side WiFi status (from /api/status, no BAPI hop). Shows
     SSID, IP, RSSI w/ visual strength bars, uptime — all the things
     you actually want to see when 'WiFi Status' was previously just
     a single 'Connected' word from the charger's gwsta. */
  var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;if(!r)return;
  r.style.display='block';
  r.innerHTML="<h2>&#x1F4F6; WiFi Status</h2><div style='text-align:center'><span class='spinner'></span> Loading...</div>";
  fetch('/api/status').then(function(x){return x.json()}).then(function(s){
    var h="<h2>&#x1F4F6; WiFi Status</h2>";
    var sBadge=s.wifi==='connected'?"<span class='badge badge-success'>Connected</span>":"<span class='badge badge-warning'>"+(s.wifi||'Unknown')+"</span>";
    h+=row('Status',sBadge);
    if(s.ssid)h+=row('Network',s.ssid);
    if(s.ip)h+=row('IP Address',s.ip);
    if(typeof s.wifi_rssi==='number'){
      var bars=s.wifi_rssi>-50?'████':s.wifi_rssi>-60?'███░':s.wifi_rssi>-70?'██░░':s.wifi_rssi>-80?'█░░░':'░░░░';
      var col=s.wifi_rssi>-60?'#22c55e':s.wifi_rssi>-70?'#f59e0b':'#ef4444';
      h+=row('Signal',"<span style='font-family:monospace;color:"+col+";letter-spacing:2px'>"+bars+"</span> "+s.wifi_rssi+" dBm");
    }
    if(typeof s.uptime==='number'){
      var u=s.uptime,hr=Math.floor(u/3600),mn=Math.floor((u%3600)/60);
      h+=row('Gateway uptime',hr>0?hr+'h '+mn+'m':mn+'m');
    }
    if(s.dev_mfg||s.dev_model)h+=row('BLE module',(s.dev_mfg||'?')+' '+(s.dev_model||''));
    if(s.dev_fw)h+=row('BLE module FW',s.dev_fw);
    r.innerHTML=h;
  }).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'})
}
var allSchedules=[];
var editingSid=null;
var DAYS_M=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
function renderCostPanel(T){
  var DAYNAMES=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];
  var h="<h2>\u{1F4B0} Charging Cost</h2>";
  h+="<label style='display:flex;align-items:center;gap:8px;cursor:pointer'><input type='checkbox' id='cost-en' "+(T.enabled?'checked':'')+" style='width:auto'> Enable cost tracking</label>";
  h+="<div class='row' style='margin-top:8px'><div><label>Currency</label><input id='cost-cur' value='"+(T.currency||'$')+"' maxlength='4' style='max-width:80px'></div><div><label>Base rate ($/kWh)</label><input type='number' id='cost-base' value='"+T.baseRate+"' min='0' max='10' step='0.01'></div></div>";
  h+="<label>Solar (green) rate ($/kWh)</label><input type='number' id='cost-green' value='"+T.greenRate+"' min='0' max='10' step='0.01'>";
  h+="<h3 style='font-size:.95em;margin:16px 0 6px;color:var(--text2)'>Tariff periods (first match wins)</h3>";
  h+="<div id='tier-list'>";
  (T.tiers||[]).forEach(function(t,i){h+=renderTierRow(t,i,DAYNAMES)});
  h+="</div>";
  h+="<button class='btn btn-outline' style='margin-top:8px;width:100%' onclick='addTier()'>+ Add Period</button>";
  h+="<div class='row' style='margin-top:14px'><button class='btn btn-success' onclick='saveCost()'>Save</button><button class='btn btn-outline' onclick='cancelCost()'>Cancel</button></div>";
  h+="<p class='help' style='margin-top:12px'>Times use the charger's configured timezone. Periods are matched top-down. If no period matches, the base rate applies.</p>";
  return h;
}
function renderTierRow(t,i,DAYNAMES){
  var h="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:6px 0'>";
  h+="<div style='display:flex;justify-content:space-between;align-items:center;gap:8px'><input value='"+(t.name||'Tier '+(i+1))+"' data-tname='"+i+"' placeholder='Name' style='flex:1;margin:0'><input type='number' value='"+t.rate+"' data-trate='"+i+"' min='0' step='0.01' style='width:90px;margin:0' placeholder='$/kWh'><button class='btn btn-outline' style='padding:6px 10px;color:var(--danger)' onclick='removeTier("+i+")'>\u2716</button></div>";
  h+="<div style='display:flex;flex-wrap:wrap;gap:4px;margin:8px 0 4px'>";
  for(var b=0;b<7;b++){var on=(t.days&(1<<b))!==0;h+="<label style='display:flex;align-items:center;gap:3px;background:"+(on?'rgba(59,130,246,.15)':'var(--elevated)')+";padding:6px 10px;border-radius:6px;cursor:pointer;font-size:.82em'><input type='checkbox' data-tday='"+i+":"+b+"' value='"+(1<<b)+"' "+(on?'checked':'')+" style='width:auto'>"+DAYNAMES[b]+"</label>"}
  h+="</div>";
  h+="<div class='row' style='margin-top:6px'><div><label style='font-size:.75em'>From hour</label><input type='number' value='"+t.fromH+"' data-tfrom='"+i+"' min='0' max='23'></div><div><label style='font-size:.75em'>To hour</label><input type='number' value='"+t.toH+"' data-tto='"+i+"' min='0' max='23'></div></div>";
  h+="</div>";
  return h;
}
function readTiersFromUI(){
  var tiers=[];
  document.querySelectorAll('[data-tname]').forEach(function(el){
    var i=parseInt(el.getAttribute('data-tname'));
    var rate=parseFloat(document.querySelector('[data-trate="'+i+'"]').value)||0;
    var fromH=parseInt(document.querySelector('[data-tfrom="'+i+'"]').value)||0;
    var toH=parseInt(document.querySelector('[data-tto="'+i+'"]').value)||0;
    var days=0;
    document.querySelectorAll('[data-tday^="'+i+':"]:checked').forEach(function(c){days+=parseInt(c.value)});
    tiers.push({name:el.value||('Tier '+(i+1)),rate:rate,days:days,fromH:fromH,toH:toH});
  });
  return tiers;
}
function addTier(){
  var T=window._editTariff;
  T.tiers=readTiersFromUI();
  T.tiers.push({name:'New period',rate:T.baseRate||0.30,days:127,fromH:0,toH:24});
  var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;
  if(r)r.innerHTML=renderCostPanel(T);
}
function removeTier(i){
  var T=window._editTariff;
  T.tiers=readTiersFromUI();
  T.tiers.splice(i,1);
  var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;
  if(r)r.innerHTML=renderCostPanel(T);
}
function saveCost(){
  var T=window._editTariff||{};
  T.enabled=document.getElementById('cost-en').checked;
  T.currency=document.getElementById('cost-cur').value||'$';
  T.baseRate=parseFloat(document.getElementById('cost-base').value)||0;
  T.greenRate=parseFloat(document.getElementById('cost-green').value)||0;
  T.tiers=readTiersFromUI();
  try{localStorage.setItem('wb-tariff',JSON.stringify(T));toast('Cost settings saved','success')}catch(e){toast('Failed to save: '+e.message,'error')}
}
function cancelCost(){
  var ap=document.querySelector('.tab-panel.active');var r=ap?ap.querySelector('[id^=qr]'):null;
  if(r){r.style.display='none';r.innerHTML=''}
}
function applyTheme(t){
  try{
    if(t==='auto'){localStorage.removeItem('wb-theme');document.documentElement.removeAttribute('data-theme')}
    else{localStorage.setItem('wb-theme',t);document.documentElement.setAttribute('data-theme',t)}
    toast('Theme: '+(t==='auto'?'Auto':t.charAt(0).toUpperCase()+t.slice(1)),'success');
  }catch(e){toast('Failed to save theme','error')}
}
var SCH_COLORS=['#3b82f6','#22c55e','#f59e0b','#a855f7','#ec4899','#06b6d4','#84cc16','#f97316'];
// Parse a HHMM UTC string into the local-time hour (0-23) in CHARGER_TZ.
function _utcHHMMtoLocalHour(hhmm){
  var s=String(hhmm).padStart(4,'0');
  var h=parseInt(s.slice(0,2)),m=parseInt(s.slice(2,4));
  try{
    var now=new Date();
    var d=new Date(Date.UTC(now.getUTCFullYear(),now.getUTCMonth(),now.getUTCDate(),h,m));
    var localStr=d.toLocaleString('en-US',{timeZone:CHARGER_TZ,hour12:false,hour:'2-digit',minute:'2-digit'});
    // localStr is e.g. "14:32" — pull hour
    return parseInt(localStr.split(':')[0])%24;
  }catch(e){return h}
}
function buildScheduleTimeline(schedules){
  var wrap=document.getElementById('sch-timeline-wrap');
  var grid=document.getElementById('sch-timeline');
  var legend=document.getElementById('sch-legend');
  if(!wrap||!grid||!legend)return;
  if(!schedules||!schedules.length){wrap.style.display='none';return}
  // Build [day][hour] -> array of schedule indexes that cover it
  var cells=[];
  for(var d=0;d<7;d++){cells.push([]);for(var h=0;h<24;h++)cells[d].push([])}
  schedules.forEach(function(s,i){
    if(!s.enabled)return;
    var fromH=_utcHHMMtoLocalHour(s.start);
    var toH=_utcHHMMtoLocalHour(s.stop);
    if(toH===fromH)toH=(fromH+24)%24;  // 24h block
    for(var d=0;d<7;d++){
      if(!(s.days&(1<<d)))continue;
      var h=fromH;
      while(h!==toH){cells[d][h].push(i);h=(h+1)%24}
    }
  });
  // Render header row: corner cell + hour ticks
  var html="<div></div>";
  for(var h=0;h<24;h++){html+="<div style='text-align:center;color:var(--text3);line-height:1'>"+(h%6===0?h:'')+"</div>"}
  // Day rows
  for(var d=0;d<7;d++){
    html+="<div style='font-weight:600;color:var(--text2);padding-right:4px;font-size:.85em'>"+DAYS_M[d]+"</div>";
    for(var h=0;h<24;h++){
      var c=cells[d][h];
      var bg='var(--elevated)';
      var title=DAYS_M[d]+' '+h+':00';
      if(c.length===1){
        bg=SCH_COLORS[c[0]%SCH_COLORS.length];
        title+=' — #'+schedules[c[0]].sid;
      }else if(c.length>1){
        // overlap: striped background using two colors
        bg='repeating-linear-gradient(45deg,'+SCH_COLORS[c[0]%SCH_COLORS.length]+' 0 4px,'+SCH_COLORS[c[1]%SCH_COLORS.length]+' 4px 8px)';
        title+=' — overlap: #'+c.map(function(i){return schedules[i].sid}).join(', #');
      }
      html+="<div title='"+title+"' style='aspect-ratio:1;background:"+bg+";border-radius:2px;min-height:10px'></div>";
    }
  }
  grid.innerHTML=html;
  // Legend
  var lg='';
  schedules.forEach(function(s,i){
    if(!s.enabled)return;
    var color=SCH_COLORS[i%SCH_COLORS.length];
    var t1=utcToLocal(s.start),t2=utcToLocal(s.stop);
    lg+="<div style='display:inline-flex;align-items:center;gap:4px;color:var(--text3)'><span style='display:inline-block;width:10px;height:10px;background:"+color+";border-radius:2px'></span>#"+s.sid+" "+t1+"–"+t2+"</div>";
  });
  legend.innerHTML=lg||'<span style=\"color:var(--text3)\">All schedules disabled</span>';
  wrap.style.display='block';
}
function loadSchedules(_retry){
  // _retry: undefined = first attempt, true = the auto-retry. We retry
  // once after a brief settle when the BAPI call times out or the
  // gateway returns null (typically because the BLE mutex was busy
  // with another command \u2014 common right after page load when the
  // BLE init sequence overlaps the schedule fetch).
  var l=document.getElementById('sch-list');
  if(!l)return;
  l.innerHTML=_retry?"<span class='spinner'></span> Retrying...":"<span class='spinner'></span> Loading...";
  fetch('/api/command?action=bapi&met=r_schs&par=null',{signal:AbortSignal.timeout(_retry?20000:15000)}).then(function(x){return x.json()}).then(function(d){
    if(d.error){
      if(!_retry){setTimeout(function(){loadSchedules(true)},1500);return}
      l.innerHTML='<span style="color:var(--danger)">'+d.error+' <button class=\'btn btn-outline\' style=\'padding:4px 8px;margin-left:8px\' onclick=\'loadSchedules()\'>Retry</button></span>';return
    }
    var sc=null;
    if(d.r&&Array.isArray(d.r.schedules))sc=d.r.schedules;
    else if(Array.isArray(d.r))sc=d.r;
    if(sc===null){
      if(!_retry){setTimeout(function(){loadSchedules(true)},1500);return}
      l.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">Couldn\u2019t load schedules (BLE may be reconnecting). <button class=\'btn btn-outline\' style=\'padding:4px 10px;margin-top:6px\' onclick=\'loadSchedules()\'>Retry</button></div>';return
    }
    allSchedules=sc;
    try{buildScheduleTimeline(sc)}catch(e){console.error('timeline failed',e)}
    if(!sc.length){l.innerHTML='<div style="color:var(--text3);text-align:center;padding:8px">No schedules yet. Tap + Add New to create one.</div>';return}
    var html='';
    sc.forEach(function(s){
      var ds='';for(var b=0;b<7;b++)if(s.days&(1<<b))ds+=DAYS_M[b]+' ';
      var t1=utcToLocal(s.start),t2=utcToLocal(s.stop);
      var lim=(s.mcr<=1||s.mcr>=32)?'No limit':s.mcr+' A';
      var ek=(s.target&&s.target.type==1)?(s.target.value/1000)+' kWh':'';
      var badge=s.enabled?'<span class=\"badge badge-success\">On</span>':'<span class=\"badge badge-warning\">Off</span>';
      html+="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:6px 0'><div style='display:flex;justify-content:space-between;align-items:flex-start;gap:8px'><div style='flex:1;min-width:0'><div style='font-weight:600;font-size:.92em'>"+t1+" \u2013 "+t2+" "+badge+"</div><div style='font-size:.78em;color:var(--text3);margin-top:3px'>"+(ds.trim()||'No days')+" \u00B7 "+lim+(ek?' \u00B7 '+ek:'')+" \u00B7 #"+s.sid+"</div></div><div style='display:flex;gap:6px;flex-shrink:0'><button class='btn btn-outline' style='padding:6px 10px;font-size:.85em' onclick='editSchedule("+s.sid+")'>\u270E</button><button class='btn btn-outline' style='padding:6px 10px;font-size:.85em;color:var(--danger)' onclick='deleteSchedule("+s.sid+")'>\u2716</button></div></div></div>";
    });
    l.innerHTML=html;
  }).catch(function(e){
    if(!_retry){setTimeout(function(){loadSchedules(true)},1500);return}
    l.innerHTML='<span style="color:var(--danger)">'+(e.message||e)+' <button class=\'btn btn-outline\' style=\'padding:4px 8px;margin-left:8px\' onclick=\'loadSchedules()\'>Retry</button></span>'
  });
}
function newSchedule(){
  editingSid=null;
  document.getElementById('sch-form-title').textContent='New Schedule';
  document.getElementById('ss').value='14:00';
  document.getElementById('se').value='20:00';
  document.querySelectorAll('#sd input').forEach(function(c){c.checked=false});
  document.getElementById('sc').value='32';
  document.getElementById('se2').value='0';
  document.getElementById('sn').value='1';
  document.getElementById('sched-edit').style.display='block';
  document.getElementById('sched-edit').scrollIntoView({behavior:'smooth',block:'start'});
}
function editSchedule(sid){
  var s=allSchedules.find(function(x){return x.sid===sid});
  if(!s){toast('Schedule not found','error');return}
  editingSid=sid;
  document.getElementById('sch-form-title').textContent='Edit Schedule #'+sid;
  document.getElementById('ss').value=utcToLocal(s.start);
  document.getElementById('se').value=utcToLocal(s.stop);
  document.querySelectorAll('#sd input').forEach(function(c){c.checked=(s.days&parseInt(c.value))!==0});
  document.getElementById('sc').value=s.mcr||32;
  document.getElementById('se2').value=(s.target&&s.target.type==1)?Math.round(s.target.value/1000):0;
  document.getElementById('sn').value=s.enabled?'1':'0';
  document.getElementById('sched-edit').style.display='block';
  document.getElementById('sched-edit').scrollIntoView({behavior:'smooth',block:'start'});
}
function cancelEdit(){
  document.getElementById('sched-edit').style.display='none';
  editingSid=null;
}
function saveSch(){
  var st=localToUtc(document.getElementById('ss').value);
  var sp=localToUtc(document.getElementById('se').value);
  var d=0;document.querySelectorAll('#sd input:checked').forEach(function(c){d+=parseInt(c.value)});
  if(!d){toast('Select at least one day','error');return}
  var mcr=parseInt(document.getElementById('sc').value);
  var ekwh=parseInt(document.getElementById('se2').value)||0;
  var tgt=ekwh>0?{type:1,value:ekwh*1000}:{type:0,value:0};
  var sid;
  if(editingSid!==null){sid=editingSid}else{var maxSid=allSchedules.length?Math.max.apply(null,allSchedules.map(function(s){return s.sid})):-1;sid=maxSid+1}
  var p=JSON.stringify({sid:sid,start:st,stop:sp,days:d,enabled:parseInt(document.getElementById('sn').value),mcr:mcr,repeat:1,type:0,name:'',target:tgt});
  toast('Saving...','info');
  fetch('/api/command?action=bapi&met=w_sch&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(15000)}).then(function(x){return x.json()}).then(function(d){
    if(d.error){toast(d.error,'error');return}
    toast(editingSid!==null?'Schedule #'+sid+' updated':'Schedule added','success');
    cancelEdit();
    loadSchedules();  }).catch(function(e){toast('Error: '+e.message,'error')});
}
function deleteSchedule(sid){
  confirm2('Delete schedule #'+sid+'?',function(){doDeleteSchedule(sid)});
}
function doDeleteSchedule(sid){
  var keep=allSchedules.filter(function(s){return s.sid!==sid});
  toast('Deleting schedule #'+sid+'...','info');
  fetch('/api/command?action=bapi&met=clr_sch&par=null',{signal:AbortSignal.timeout(15000)}).then(function(x){return x.json()}).then(function(){
    var i=0;
    function next(){
      if(i>=keep.length){toast('Deleted','success');loadSchedules();return}
      var s=keep[i++];
      var p=JSON.stringify({sid:i-1,start:s.start,stop:s.stop,days:s.days,enabled:s.enabled,mcr:s.mcr,repeat:s.repeat||1,type:s.type||0,name:s.name||'',target:s.target||{type:0,value:0}});
      fetch('/api/command?action=bapi&met=w_sch&par='+encodeURIComponent(p),{signal:AbortSignal.timeout(15000)}).then(function(x){return x.json()}).then(next).catch(next);
    }
    next();
  }).catch(function(e){toast('Error: '+e.message,'error');loadSchedules()});
}
function delSch(){confirm2('Delete ALL schedules?',function(){fetch('/api/command?action=bapi&met=clr_sch&par=null').then(function(x){return x.json()}).then(function(){toast('All schedules deleted','success');loadSchedules()}).catch(function(e){toast('Error: '+e.message,'error')})});}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
var _pauseTimer=null;
function fmtMmSs(s){var m=Math.floor(s/60),ss=s%60;return m+':'+(ss<10?'0':'')+ss}
function startPauseUI(remaining){
  var btn=document.getElementById('ble-pause-btn');
  if(!btn)return;
  btn.disabled=true;
  btn.style.background='rgba(245,158,11,.15)';
  btn.style.borderColor='#f59e0b';
  btn.style.color='#f59e0b';
  if(_pauseTimer)clearInterval(_pauseTimer);
  var deadline=Date.now()+remaining*1000;
  function tick(){
    var s=Math.max(0,Math.round((deadline-Date.now())/1000));
    btn.innerHTML="⏸ BLE released · "+fmtMmSs(s)+" remaining";
    if(s<=0){clearInterval(_pauseTimer);_pauseTimer=null;btn.disabled=false;btn.style.background='';btn.style.borderColor='';btn.style.color='';btn.innerHTML='\u{1F4F4} Release BLE for App';toast('BLE resumed','info')}
  }
  tick();_pauseTimer=setInterval(tick,1000);
}
function pauseBle(){
  confirm2('Release BLE for 5 minutes? Gateway will stop polling so the official Wallbox app can connect.',function(){
    var btn=document.getElementById('ble-pause-btn');if(btn){btn.disabled=true;btn.innerHTML="<span class='spinner'></span> Releasing..."}
    fetch('/api/ble/pause?ms=300000').then(function(x){return x.json()}).then(function(d){
      var s=d.paused_for_s||300;
      toast('BLE released — open the Wallbox app now','success');
      startPauseUI(s);
    }).catch(function(e){toast('Error: '+e.message,'error');if(btn){btn.disabled=false;btn.innerHTML='\u{1F4F4} Release BLE for App'}});
  });
}
// Defer the on-load fetches until window.onload fires (i.e. AFTER
// /app.js + /style.css + /manifest.json have all finished loading).
// Was firing at script-parse time, which ran the heavy r_schs BAPI
// call in parallel with the static-asset GETs. Concurrent ESP32
// HTTP responses each allocate ~10–20 KB of heap; min-heap was
// dipping into the malloc-failure / panic zone (~30–60 KB free).
// Deferring trades ~200 ms of perceived load time for reliable
// no-panic page loads.
window.addEventListener('load',function(){
  fetch('/api/status').then(function(x){return x.json()}).then(function(d){
    if(d.ble_paused&&d.ble_pause_remaining>0)startPauseUI(d.ble_pause_remaining);
  }).catch(function(){});
  loadSchedules();
});
</script>
</div>
)HTML");
    http.sendContent(htmlFoot("/settings"));
    http.sendContent("");  // final empty chunk terminates chunked encoding
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
    page += "<label>Bluetooth Passcode <span style='color:var(--text3);font-weight:400'>(also called \"BLE PIN\")</span></label><input name='ble_pin' value='" + cfg.blePin + "' placeholder='Leave blank if your firmware has no passcode'>";
    page += "<p class='help'>For Pulsar Plus / newer Pulsar MAX firmware (6.11+): open the Wallbox app, go to your charger → Settings, and look for <b>Bluetooth Passcode</b>. Copy that 8-digit code here. On older firmware leave blank.</p></div>";

    // Security
    page += "<div class='card'><div class='card-header'><span class='card-icon'>&#x1F512;</span><h2>Web Security</h2></div>";
    if (!cfg.authEnabled || cfg.authPass.length() == 0) {
        page += "<div style='background:rgba(245,158,11,.10);border:1px solid rgba(245,158,11,.30);border-radius:8px;padding:10px;margin-bottom:10px;font-size:.85em;color:#f59e0b'>";
        page += "&#x26A0; <b>No password set.</b> Anyone on your local network can control the charger via this UI. ";
        page += "Set a password below (we'll enable auth automatically when you save).";
        page += "</div>";
    }
    page += "<label>Enable Authentication</label>";
    page += "<select name='auth_en'><option value='0'" + String(cfg.authEnabled ? "" : " selected") + ">Disabled</option><option value='1'" + String(cfg.authEnabled ? " selected" : "") + ">Enabled</option></select>";
    page += "<div class='row'>";
    page += "<div><label>Username</label><input name='auth_user' value='" + cfg.authUser + "'></div>";
    page += "<div><label>Password <span style='color:var(--text3);font-weight:400'>(recommended)</span></label><input type='password' name='auth_pass' value='" + cfg.authPass + "' placeholder='Leave blank to skip — local network only'></div>";
    page += "</div>";
    page += "<p class='help'>When enabled, all control actions and OTA require login. Dashboard viewing remains open. If you set a password and leave auth disabled, we'll enable it for you.</p></div>";

    // Advanced
    page += "<details><summary style='color:var(--text3);cursor:pointer;padding:6px 0;font-size:.85em'>Advanced</summary><div class='card'>";
    page += "<label>Charger model</label><select name='chg_model'>";
    page += String("<option value='max'") + (cfg.chargerModel == "max" ? " selected" : "") + ">Pulsar MAX (single-char)</option>";
    page += String("<option value='plus'") + (cfg.chargerModel == "plus" ? " selected" : "") + ">Pulsar Plus (dual-char)</option>";
    page += String("<option value='copper'") + (cfg.chargerModel == "copper" ? " selected" : "") + ">Copper SB (Plus protocol — experimental)</option>";
    page += String("<option value='quasar'") + (cfg.chargerModel == "quasar" ? " selected" : "") + ">Quasar (Plus protocol — experimental)</option>";
    page += String("<option value='quasar2'") + (cfg.chargerModel == "quasar2" ? " selected" : "") + ">Quasar 2 / V2H (Plus protocol — experimental)</option>";
    page += String("<option value='custom'") + (cfg.chargerModel == "custom" ? " selected" : "") + ">Custom (set UUIDs below)</option>";
    page += "</select>";
    page += "<p class='help'>Picking <b>Pulsar Plus</b> auto-fills the Nordic UART UUIDs on save. Use <b>Custom</b> if you know better.</p>";
    page += "<label>Service UUID</label><input name='ble_svc' value='" + cfg.bleService + "' style='font-size:12px;font-family:monospace'>";
    page += "<label>Char UUID (write — also notify in single-char mode)</label><input name='ble_chr' value='" + cfg.bleChar + "' style='font-size:12px;font-family:monospace'>";
    page += "<label>TX/Notify Char UUID (optional — leave blank for single-char Pulsar MAX)</label><input name='ble_txchr' value='" + cfg.bleTxChar + "' placeholder='Required for Pulsar Plus' style='font-size:12px;font-family:monospace'>";
    page += "<div class='row'><div><label>Status Poll (ms)</label><input name='poll_status' type='number' value='" + String(cfg.statusPollMs) + "'></div>";
    page += "<div><label>Realtime Poll (ms)</label><input name='poll_rt' type='number' value='" + String(cfg.realtimePollMs) + "'></div></div>";
    page += "<div class='row'><div><label>HA Prefix</label><input name='ha_prefix' value='" + cfg.haDiscoveryPrefix + "'></div>";
    page += "<div><label>Device ID</label><input name='ha_devid' value='" + cfg.haDeviceId + "'></div></div></div></details>";

    page += "<button type='submit' class='btn btn-success' style='margin-top:12px'>&#x1F4BE; Save &amp; Reboot</button></form>";
    page += "<a href='/ota' class='btn btn-outline' style='margin-top:10px'>&#x1F4E6; Firmware Update</a>";

    // Backup & Restore — passwords/PINs are masked in the export, so the
    // download is safe to share. Restore preserves the existing secrets
    // when a field is "***" in the upload.
    page += "<div class='card' style='margin-top:14px'><div class='card-header'><span class='card-icon'>&#x1F4BE;</span><h2>Backup &amp; Restore</h2></div>";
    page += "<p class='help' style='margin-bottom:10px'>Download saves the current config as JSON (passwords masked). Restore applies a previously saved config and reboots.</p>";
    page += "<a href='/api/config/export' download='wallbox-config.json' class='btn btn-outline' style='margin-right:8px;text-decoration:none'>&#x2B07; Download config</a>";
    page += "<label style='margin-top:14px'>Restore from file</label>";
    page += "<input type='file' id='cfg-file' accept='.json,application/json' style='margin-bottom:10px'>";
    page += "<button type='button' class='btn btn-outline' onclick='doRestore()'>&#x2B06; Restore &amp; Reboot</button>";
    page += "<p id='restore-status' style='font-size:.82em;color:var(--text3);margin-top:8px'></p>";
    page += "<script>function doRestore(){var f=document.getElementById('cfg-file').files[0];var st=document.getElementById('restore-status');if(!f){st.textContent='Pick a file first.';st.style.color='var(--warning)';return}var r=new FileReader();r.onload=function(){fetch('/api/config/import?csrf=" + csrfToken + "',{method:'POST',headers:{'Content-Type':'application/json'},body:r.result}).then(function(resp){return resp.json()}).then(function(d){if(d.ok){st.textContent='Imported — rebooting...';st.style.color='var(--success)'}else{st.textContent='Failed: '+(d.error||'unknown');st.style.color='var(--danger)'}}).catch(function(e){st.textContent='Error: '+e;st.style.color='var(--danger)'})};r.readAsText(f)}</script>";
    page += "</div>";

    // Reboot (keeps config) — useful for capturing a fresh boot trace
    page += "<button type='button' class='btn btn-outline' style='margin-top:10px' onclick='confirm2(\"Reboot the gateway? Config is preserved.\",function(){fetch(\"/api/reboot?csrf=" + csrfToken + "\",{method:\"POST\"}).then(function(){location.href=\"/logs\"}).catch(function(){})})'>&#x21BB; Reboot Gateway</button>";

    page += "<button type='button' class='btn btn-danger' style='margin-top:10px' onclick='confirm2(\"Erase all settings and reboot into setup mode?\",function(){var f=document.createElement(\"form\");f.method=\"POST\";f.action=\"/reset\";var i=document.createElement(\"input\");i.type=\"hidden\";i.name=\"csrf\";i.value=\"" + csrfToken + "\";f.appendChild(i);document.body.appendChild(f);f.submit()})'>&#x1F5D1; Factory Reset</button>";

    page += htmlFoot("/config");
    http.send(200, "text/html", page);
}

// ========== PAGE 4: Info (/info) ==========
static void handleInfo() {
    String page = htmlHead("Info");
    // CSRF token needed by interactive JS (e.g. clearDiag) — injected
    // here so the page-body raw string can stay static.
    page += "<script>window.WB_CSRF='" + csrfToken + "';</script>";
    page += R"HTML(
<div class='loading' id='ld'><div class='ld-spin'></div>Loading Info...</div>
<div id='pg' style='display:none'>
<h1>&#x2139; Gateway Info</h1>

<div id='fw-changed-banner' style='display:none;background:rgba(245,158,11,.10);border:1px solid rgba(245,158,11,.30);border-radius:8px;padding:10px;margin-bottom:12px;font-size:.88em;color:#f59e0b'>
  &#x1F535; <b>Charger firmware changed.</b> <span id='fw-changed-detail'></span>
  Behaviour may have shifted — keep an eye on the dashboard. <a href='#' onclick='dismissFwBanner();return false' style='color:#f59e0b;text-decoration:underline'>Dismiss</a>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4E1;</span><h2>Gateway</h2></div>
  <div id='gw'>Loading...</div>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F50C;</span><h2>Charger Details</h2></div>
  <div id='chg'>Loading...</div>
</div>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4C8;</span><h2>Connection Diagnostics</h2></div>
  <div id='diag-rows' style='font-size:.88em'>Loading...</div>
  <button class='btn btn-outline' style='padding:6px 12px;font-size:.82em;margin-top:8px' onclick='clearDiag()'>Clear counters</button>
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
  <a href='/ota' class='btn btn-outline' style='text-decoration:none;display:block;margin-bottom:8px'>&#x1F4E6; Firmware Update</a>
  <div id='boot-reason' style='font-size:.82em;color:var(--text3);margin-top:6px;margin-bottom:6px'></div>
  <div id='ota-history' style='display:none;margin-top:8px'>
    <div style='font-size:.82em;color:var(--text2);margin-bottom:6px'>Recent OTA attempts:</div>
    <div id='ota-history-rows' style='font-size:.78em;font-family:monospace'></div>
  </div>
</div>
<p style='text-align:center;color:var(--text3);font-size:.75em;margin-top:16px'>Wallbox BLE Gateway )HTML" WB_VERSION R"HTML(</p>

<script>
function _sect(title){return "<div style='margin-top:10px;color:var(--text3);font-size:.72em;text-transform:uppercase;letter-spacing:.06em;border-bottom:1px solid var(--border);padding-bottom:3px;margin-bottom:4px;font-weight:600'>"+title+"</div>"}
function loadGW(){return fetch('/api/status').then(function(r){return r.json()}).then(function(d){
  window._lastStatus=d;renderGW(d);
  // /info Charger Details card — grouped into sections for readability.
  // Each section is hidden if none of its rows have data, so an
  // unconfigured / un-paired charger doesn't show empty headers.
  var c='', s='';
  // Identity
  s='';
  if(d.dev_name)s+=row('Name',d.dev_name);
  if(d.chg_sn)s+=row('Serial Number',d.chg_sn);
  if(d.chg_mac)s+=row('MAC',d.chg_mac);
  if(s){c+=_sect('Identity')+s}
  // Firmware
  s='';
  if(d.chg_app_fw)s+=row('Charger',d.chg_app_fw);
  if(d.chg_project)s+=row('Project',d.chg_project);
  if(s){c+=_sect('Firmware')+s}
  // Operation
  s='';
  if(d.chg_sessions>=0)s+=row('Total Sessions',d.chg_sessions);
  if(d.chg_power_boost>=0)s+=row('Power Boost',d.chg_power_boost+' A');
  if(d.chg_lock_state>=0)s+=row('Lock',d.chg_lock_state===1?'Locked':'Unlocked');
  if(d.chg_grounding)s+=row('Grounding',d.chg_grounding);
  if(s){c+=_sect('Operation')+s}
  // Charger Network (the charger's own WiFi link, not our gateway's)
  s='';
  if(d.chg_net_ssid)s+=row('WiFi',d.chg_net_ssid);
  if(d.chg_net_ip)s+=row('IP',d.chg_net_ip);
  if(typeof d.chg_net_signal==='number'&&d.chg_net_signal>0)s+=row('Signal',d.chg_net_signal+' %');
  if(s){c+=_sect('Charger Network')+s}
  // BLE Module
  s='';
  if(d.dev_mfg)s+=row('Manufacturer',d.dev_mfg);
  if(d.dev_model)s+=row('Model',d.dev_model);
  if(d.dev_fw)s+=row('Firmware',d.dev_fw);
  if(s){c+=_sect('BLE Module')+s}
  // Gateway WiFi — the ESP32's link to the user's network. Many users
  // care more about this signal than the charger's own WiFi.
  s='';
  if(d.ssid)s+=row('WiFi',d.ssid);
  if(d.ip)s+=row('IP',d.ip);
  if(typeof d.wifi_rssi==='number')s+=row('Signal',d.wifi_rssi+' dBm');
  if(s){c+=_sect('Gateway WiFi')+s}
  document.getElementById('chg').innerHTML=c||'<span style="color:var(--text3)">Connect BLE to see charger details</span>';
  if(d.chg_fw_changed){var b=document.getElementById('fw-changed-banner');var det=document.getElementById('fw-changed-detail');if(b){b.style.display='block';if(det&&d.chg_fw_prev&&d.dev_fw)det.textContent='Was '+d.chg_fw_prev+', now '+d.dev_fw+'.';}}
}).catch(function(){})}
function dismissFwBanner(){fetch('/api/fw/dismiss',{method:'POST'}).then(function(){var b=document.getElementById('fw-changed-banner');if(b)b.style.display='none'}).catch(function(){})}
function loadOtaHistory(){return fetch('/api/ota/history').then(function(r){return r.json()}).then(function(arr){if(!Array.isArray(arr)||!arr.length)return;var c=document.getElementById('ota-history');var rows=document.getElementById('ota-history-rows');var h='';arr.forEach(function(e){var dot=e.ok?'<span style="color:#22c55e">&#x25CF;</span>':'<span style="color:#ef4444">&#x25CF;</span>';var sz=e.bytes?(' '+Math.round(e.bytes/1024)+'KB'):'';var rsn=e.ok?'':(' — '+(e.reason||'failed'));h+='<div style="margin:3px 0">'+dot+' '+(e.from||'unknown')+sz+rsn+'</div>'});rows.innerHTML=h;c.style.display='block'}).catch(function(){})}
function loadBootReason(){return fetch('/api/boot/history').then(function(r){return r.json()}).then(function(d){var el=document.getElementById('boot-reason');if(!el)return;var cur=d.current||'unknown';var curFw=d.current_fw||'';var isBad=function(r){r=r||'';return r.indexOf('panic')>=0||r.indexOf('watchdog')>=0||r.indexOf('brownout')>=0};var bad=isBad(cur);var col=bad?'#ef4444':'var(--text3)';var prefix=bad?'&#x26A0; ':'';el.innerHTML='<span style=\"color:'+col+'\">'+prefix+'Last boot: '+cur+'</span>';if(d.history&&d.history.length>1){var thisFw=d.history.filter(function(e){return isBad(e.reason)&&e.fw===curFw});var olderFw=d.history.filter(function(e){return isBad(e.reason)&&e.fw!==curFw});if(thisFw.length){el.innerHTML+=' <span style=\"color:var(--danger);font-size:.92em\">('+thisFw.length+' bad boot'+(thisFw.length>1?'s':'')+' on this firmware)</span>'}else if(olderFw.length){el.innerHTML+=' <span style=\"color:var(--text3);font-size:.85em;opacity:.7\">('+olderFw.length+' from older firmware)</span>'}}}).catch(function(){})}
// Chain /info fetches sequentially instead of firing 4-6 in parallel —
// ESP32's WebServer has very limited concurrent connection slots, and
// flooding it under load was causing /app.js to stall and (under worse
// load) full panic crashes. Each fetch is small (<10 KB) so sequential
// total time is still <2s on a healthy gateway, much better than
// "looks fast then explodes".
function loadInfoChained(){loadGW().then(loadBootReason).then(loadOtaHistory).then(loadDiag).catch(function(){})}
loadInfoChained();setInterval(function(){loadGW().then(loadDiag).catch(function(){})},15000);
function fmtUptime(s){if(!s)return 'never';var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);return (d?d+'d ':'')+(h?h+'h ':'')+m+'m'}
function fmtDur(s){if(s<60)return s+'s';if(s<3600)return Math.round(s/60)+'m '+(s%60)+'s';return Math.round(s/3600)+'h '+Math.round((s%3600)/60)+'m'}
function loadDiag(){
  // Fetch /api/diag/disconnects AND /api/health in parallel — disconnect
  // counters plus runtime-health tripwires (loop_max_ms, max_reentry,
  // tokens, heap_free) in a single Diagnostics card. The latter were
  // previously only visible via curl; now they're on /info so wedge /
  // reentrancy regressions are spotable without a shell.
  return Promise.all([
    fetch('/api/diag/disconnects').then(function(r){return r.json()}).catch(function(){return{}}),
    fetch('/api/health').then(function(r){return r.json()}).catch(function(){return{}})
  ]).then(function(results){
    var d=results[0]||{}, h2=results[1]||{};
    var rows=document.getElementById('diag-rows');if(!rows)return;
    var curUp=d.uptime_s||h2.uptime||0;
    var h='';
    // Runtime health section — the wedge / reentrancy tripwires.
    if(typeof h2.max_reentry==='number'||typeof h2.loop_max_ms==='number'){
      h+=_sect('Runtime Health');
      if(typeof h2.max_reentry==='number'){
        var rcol = h2.max_reentry > 1 ? '#ef4444' : 'var(--text)';
        h+=row('Max reentry depth',"<span style='color:"+rcol+"'>"+h2.max_reentry+(h2.max_reentry>1?' &#x26A0;':'')+"</span>");
      }
      if(typeof h2.loop_max_ms==='number'){
        var lcol = h2.loop_max_ms > 500 ? '#f59e0b' : (h2.loop_max_ms > 2000 ? '#ef4444' : 'var(--text)');
        h+=row('Longest loop iteration',"<span style='color:"+lcol+"'>"+h2.loop_max_ms+' ms'+(h2.loop_max_ms>2000?' &#x26A0;':'')+"</span>");
      }
      if(typeof h2.tokens==='number')h+=row('Rate-limit tokens',h2.tokens+' / 4');
      if(typeof h2.heap_free==='number')h+=row('Heap free',(h2.heap_free/1024).toFixed(1)+' KB');
    }
    // Existing disconnect counters / events.
    h+=_sect('Reconnect counters');
    h+=row('BLE reconnects (this boot)',d.ble_reconnects+(d.ble_longest_s?' (longest '+fmtDur(d.ble_longest_s)+')':''));
    h+=row('MQTT reconnects (this boot)',d.mqtt_reconnects+(d.mqtt_longest_s?' (longest '+fmtDur(d.mqtt_longest_s)+')':''));
    if(d.ble_last_at_s)h+=row('Last BLE reconnect',fmtUptime(d.ble_last_at_s)+' after boot');
    if(d.mqtt_last_at_s)h+=row('Last MQTT reconnect',fmtUptime(d.mqtt_last_at_s)+' after boot');
    if(d.events&&d.events.length){
      var thisBoot=d.events.filter(function(e){return e.start<=curUp});
      var prior=d.events.filter(function(e){return e.start>curUp});
      if(thisBoot.length){
        h+='<div style="margin-top:8px;font-size:.82em;color:var(--text2)">Events this boot:</div>';
        thisBoot.slice(0,8).forEach(function(e){var kc=e.kind==='ble'?'#a78bfa':'#22d3ee';h+='<div style="font-family:monospace;font-size:.78em;margin:2px 0"><span style="color:'+kc+'">'+e.kind.toUpperCase().padEnd(4,' ')+'</span> at +'+fmtUptime(e.start)+', down '+fmtDur(e.dur)+'</div>'});
      }
      if(prior.length){
        h+='<div style="margin-top:8px;font-size:.82em;color:var(--text3)">From prior boots (NVS-persisted):</div>';
        prior.slice(0,8).forEach(function(e){var kc=e.kind==='ble'?'#a78bfa':'#22d3ee';h+='<div style="font-family:monospace;font-size:.78em;margin:2px 0;opacity:.55"><span style="color:'+kc+'">'+e.kind.toUpperCase().padEnd(4,' ')+'</span> at +'+fmtUptime(e.start)+' of that boot, down '+fmtDur(e.dur)+'</div>'});
      }
    }
    rows.innerHTML=h||'<div style="color:var(--text3)">No diagnostics logged yet.</div>';
  }).catch(function(){})
}
function clearDiag(){if(!confirm('Reset disconnect counters and clear event history?'))return;fetch('/api/diag/clear?csrf='+window.WB_CSRF,{method:'POST'}).then(function(){loadDiag()}).catch(function(){})}
function renderGW(d){var h='';h+=row('WiFi',d.ssid+' ('+d.ip+')');h+=row('WiFi Signal',d.wifi_rssi+' dBm');h+=row('BLE State',d.ble);h+=row('BLE Signal',d.rssi+' dBm');h+=row('Commands Sent',d.tx);h+=row('Responses',d.rx);var m=Math.floor(d.uptime/60),hr=Math.floor(m/60);h+=row('Uptime',hr+'h '+m%60+'m');h+=row('Free Memory',Math.round(d.heap/1024)+' KB');document.getElementById('gw').innerHTML=h}
// Live-update the Gateway card off the same WS push the top banner uses,
// so BLE Signal here can't drift away from the banner's value.
if(window.wbws){window.wbws.subscribe('ble',function(d){if(!window._lastStatus)return;window._lastStatus.ble=d.state;window._lastStatus.rssi=d.rssi;renderGW(window._lastStatus)});}
var OCPP_LABELS={chid:'Charger ID',e:'Enabled',pw:'Password',u:'Server URL',ws:'WebSocket',id:'Identity',status:'Status',connected:'Connected',protocol:'Protocol',interval:'Heartbeat (s)',auth:'Auth Type'};
var NET_LABELS={channel:'WiFi Channel',dns1:'DNS Primary',dns2:'DNS Secondary',gateway:'Gateway',ip:'IP Address',netmask:'Subnet Mask',mac:'MAC Address',ssid:'Network Name',rssi:'Signal (dBm)',signal:'Signal',status:'Status',type:'Connection Type'};
function Q(m,l){var r=document.getElementById('qr');r.style.display='block';r.innerHTML="<span class='spinner'></span>"+l+"...";fetch('/api/command?action=bapi&met='+m+'&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){if(d.error){r.innerHTML='<span style="color:var(--danger)">'+d.error+'</span>';return}var v=d.r||d;var h="<div style='font-weight:600;color:var(--accent);margin-bottom:6px'>"+l+"</div>";if(m==='gwsta'){var ws={0:'Disconnected',1:'Connected',2:'Connecting'};h+=row('WiFi Status',typeof v==='number'?(ws[v]||'Code '+v):''+v)}else if(m==='r_not'){if(Array.isArray(v)){if(v.length===0)h+=row('Notifications','None');else v.forEach(function(n,i){h+="<div style='background:var(--bg);border-radius:8px;padding:8px;margin:4px 0'>";h+=row('#'+(i+1),n.message||n.msg||JSON.stringify(n));h+="</div>"})}else{h+=row('Notifications',typeof v==='number'?(v===0?'None':''+v):''+v)}}else if(m==='g_ocpp'){var lb=OCPP_LABELS;if(typeof v==='object'){for(var k in v){var lbl=lb[k]||k;var val=v[k];if(val===null||val===undefined||val==='')val='<span style="color:var(--text3)">Not set</span>';else if(typeof val==='number'&&(k==='e'||k==='connected'))val=val?'Yes':'No';h+=row(lbl,val)}}else{h+=row('OCPP',v===1?'Connected':v===0?'Not configured':'Code '+v)}}else if(m==='gnsta'){if(Array.isArray(v)&&v.length>0){var n=v[0];var lb=NET_LABELS;for(var k in n){var lbl=lb[k]||k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});var val=n[k];if(val===''||val===null)val='<span style="color:var(--text3)">-</span>';h+=row(lbl,val)}}else if(typeof v==='object'&&!Array.isArray(v)){var lb=NET_LABELS;for(var k in v){var lbl=lb[k]||k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});var val=v[k];if(val===''||val===null)val='<span style="color:var(--text3)">-</span>';h+=row(lbl,val)}}else{h+=row('Network',''+v)}}else if(m==='gupdc'){if(typeof v==='object'){if(v.update)h+=row('Update Available','<span style="color:var(--success)">Yes</span>');else h+=row('Update Available','No');if(v.version||v.current)h+=row('Current Version',v.version||v.current||'Unknown');if(v.latest||v.new_version)h+=row('Latest Version',v.latest||v.new_version||'Unknown')}else{h+=row('Firmware',typeof v==='number'?(v===0?'Up to date':'Update available ('+v+')'):''+v)}}else if(m==='r_not'){if(typeof v==='object'){if(Array.isArray(v)){if(v.length===0)h+=row('Notifications','None');v.forEach(function(n,i){h+="<div style='background:var(--bg);border-radius:8px;padding:8px;margin:4px 0'>";h+=row('Notification '+(i+1),n.message||n.msg||n.text||JSON.stringify(n));if(n.timestamp||n.ts)h+=row('Time',new Date((n.timestamp||n.ts)*1000).toLocaleString());h+="</div>"})}else{for(var k in v){var lbl=k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});h+=row(lbl,typeof v[k]==='object'?JSON.stringify(v[k]):v[k])}}}else{h+=row('Notifications',v===0?'None':''+v)}}else if(typeof v==='object'){for(var k in v){var lbl=k.replace(/_/g,' ').replace(/\b\w/g,function(c){return c.toUpperCase()});var val=v[k];if(typeof val==='boolean')val=val?'Yes':'No';else if(typeof val==='object')val=JSON.stringify(val);h+=row(lbl,val)}}else{h+=row(l,v)}r.innerHTML=h}).catch(function(e){r.innerHTML='<span style="color:var(--danger)">'+e.message+'</span>'})}
function B(){var m=document.getElementById('bm').value,r=document.getElementById('br');r.style.display='block';r.textContent='Sending '+m+'...';fetch('/api/command?action=bapi&met='+encodeURIComponent(m)+'&par=null',{signal:AbortSignal.timeout(12000)}).then(function(x){return x.json()}).then(function(d){r.textContent=JSON.stringify(d,null,2)}).catch(function(e){r.textContent='Error: '+e.message})}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
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
    // Convenience: if the user typed a password but didn't flip the toggle, turn auth on for them.
    if (!cfg.authEnabled && cfg.authPass.length() > 0) cfg.authEnabled = true;
    cfg.bleAddr = normalizeMAC(http.arg("ble_addr")); cfg.blePin = http.arg("ble_pin");
    cfg.bleService = http.arg("ble_svc"); cfg.bleChar = http.arg("ble_chr");
    cfg.bleTxChar = http.arg("ble_txchr");
    cfg.chargerModel = http.arg("chg_model");
    // Apply preset UUIDs if a model is selected (custom = leave fields as-typed).
    // Copper SB / Quasar / Quasar 2 use the same dual-char Plus protocol per
    // jagheterfredrik/wallbox-mqtt-bridge (supports Plus + Copper SB with the
    // same BAPI surface; Quasar shares the BLE family and adds V2H state codes).
    if (cfg.chargerModel == "max") {
        cfg.bleService = "2456e1b9-26e2-8f83-e744-f34f01e9d701";
        cfg.bleChar    = "2456e1b9-26e2-8f83-e744-f34f01e9d703";
        cfg.bleTxChar  = "";  // single-char mode
    } else if (cfg.chargerModel == "plus" || cfg.chargerModel == "copper"
            || cfg.chargerModel == "quasar" || cfg.chargerModel == "quasar2") {
        // Nordic UART-style variant per jagheterfredrik/wallbox-ble
        cfg.bleService = "331a36f5-2459-45ea-9d95-6142f0c4b307";
        cfg.bleChar    = "a9da6040-0823-4995-94ec-9ce41ca28833";  // RX (write)
        cfg.bleTxChar  = "a73e9a10-628f-4494-a099-12efaf72258f";  // TX (notify)
    }
    cfg.statusPollMs = http.arg("poll_status").toInt();
    cfg.realtimePollMs = http.arg("poll_rt").toInt();
    cfg.haDiscoveryPrefix = http.arg("ha_prefix"); cfg.haDeviceId = http.arg("ha_devid");
    if (cfg.mqttPort == 0) cfg.mqttPort = 1883;
    if (cfg.statusPollMs < 1000) cfg.statusPollMs = 10000;
    if (cfg.realtimePollMs < 1000) cfg.realtimePollMs = 30000;
    configMgr.save();

    String page = htmlHead("Saved");
    page += "<div class='card' style='text-align:center'><h2 style='color:var(--success)'>&#x2705; Saved!</h2>";
    page += "<p id='wait-msg' style='color:var(--text2);margin-top:10px'>Rebooting gateway...</p>";
    page += "<div class='spinner' style='margin:16px auto'></div>";
    page += "<p id='wait-detail' style='color:var(--text3);font-size:.82em;margin-top:8px'>Waiting for it to come back online — this page will reload when it does.</p>";
    page += "</div>";
    // Poll the gateway every 2s until it responds, then redirect to the
    // dashboard. Gives ~2 min of patience for slow reboots; after that
    // shows a manual link so the user isn't stuck on the spinner forever.
    page += "<script>(function(){"
            "var n=0,max=60,started=Date.now();"
            "var msg=document.getElementById('wait-msg');"
            "var det=document.getElementById('wait-detail');"
            "function tick(){"
              "n++;"
              "var s=Math.round((Date.now()-started)/1000);"
              "fetch('/api/status',{cache:'no-store'}).then(function(r){"
                "if(r.ok){if(msg)msg.textContent='Back online \\u2014 redirecting...';location.replace('/');}"
                "else throw new Error();"
              "}).catch(function(){"
                "if(det)det.textContent='Still waiting... ('+s+'s)';"
                "if(n<max)setTimeout(tick,2000);"
                "else{if(msg)msg.textContent='Gateway not responding';"
                "if(det)det.innerHTML='Try the <a href=\"/\">dashboard</a> manually \\u2014 if that fails, the gateway may need a power cycle.';}"
              "});"
            "}"
            "setTimeout(tick,5000);"
            "})();</script>";
    page += "</body></html>";
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
<h1>&#x1F4CA; Charging Sessions<span id='sess-total' style='font-size:.55em;vertical-align:middle;margin-left:10px;padding:3px 9px;border-radius:10px;background:rgba(59,130,246,.12);color:var(--accent);font-weight:500;display:none'></span></h1>
<script>fetch('/api/status').then(function(r){return r.json()}).then(function(d){if(typeof d.chg_sessions==='number'&&d.chg_sessions>=0){var el=document.getElementById('sess-total');if(el){el.textContent=d.chg_sessions+' total';el.style.display='inline-block'}}}).catch(function(){})</script>
<p class='subtitle'>Charging history and patterns</p>

<div class='card'>
  <div class='card-header'><span class='card-icon'>&#x1F4CA;</span><h2>Totals</h2></div>
  <div style='display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px'>
    <div style='background:var(--bg);border-radius:8px;padding:10px;text-align:center'>
      <div style='font-size:.68em;color:var(--text3);text-transform:uppercase;letter-spacing:.5px'>All Time</div>
      <div id='tile-allt' style='font-size:1.1em;font-weight:600;margin-top:4px'>–</div>
    </div>
    <div style='background:var(--bg);border-radius:8px;padding:10px;text-align:center'>
      <div style='font-size:.68em;color:var(--text3);text-transform:uppercase;letter-spacing:.5px'>This Week</div>
      <div id='tile-week' style='font-size:1.1em;font-weight:600;margin-top:4px'>–</div>
    </div>
    <div style='background:var(--bg);border-radius:8px;padding:10px;text-align:center'>
      <div style='font-size:.68em;color:var(--text3);text-transform:uppercase;letter-spacing:.5px'>This Month</div>
      <div id='tile-month' style='font-size:1.1em;font-weight:600;margin-top:4px'>–</div>
    </div>
  </div>
  <div id='cost-row' style='display:none;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px'>
    <div style='background:var(--bg);border-radius:8px;padding:10px;text-align:center'>
      <div style='font-size:.68em;color:var(--text3);text-transform:uppercase;letter-spacing:.5px'>Week Cost</div>
      <div id='tile-week-cost' style='font-size:1.1em;font-weight:600;margin-top:4px;color:#22c55e'>–</div>
      <div id='tile-week-saved' style='font-size:.72em;color:#fbbf24;margin-top:2px'></div>
    </div>
    <div style='background:var(--bg);border-radius:8px;padding:10px;text-align:center'>
      <div style='font-size:.68em;color:var(--text3);text-transform:uppercase;letter-spacing:.5px'>Month Cost</div>
      <div id='tile-month-cost' style='font-size:1.1em;font-weight:600;margin-top:4px;color:#22c55e'>–</div>
      <div id='tile-month-saved' style='font-size:.72em;color:#fbbf24;margin-top:2px'></div>
    </div>
  </div>
  <div style='text-align:center;font-size:.7em;color:var(--text3);margin-top:8px'>Long-term graphs: use the HA Energy dashboard</div>
</div>

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
  <div class='card-header'><span class='card-icon'>&#x1F4C8;</span><h2>Daily Charging</h2></div>
  <div id='slist'><span class='spinner'></span> Loading sessions...</div>
  <button class='btn btn-outline' style='margin-top:12px;width:100%' onclick='exportCsv()'>&#x1F4E5; Export CSV</button>
</div>

<p style='text-align:center;margin-top:16px'>
  <a href='/settings' style='color:var(--text3)'>&#x2190; Back to Settings</a>
</p>
</div>
<script>
var DAYS=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
var CHARGER_TZ='UTC';
try{CHARGER_TZ=Intl.DateTimeFormat().resolvedOptions().timeZone||'UTC'}catch(e){}
fetch('/api/command?action=bapi&met=g_tzn&par=null',{signal:AbortSignal.timeout(8000)}).then(function(r){return r.json()}).then(function(d){if(d.r&&d.r.timezone)CHARGER_TZ=d.r.timezone}).catch(function(){});
function buildHeatmap(sessions){
  // day[0-6] × hour[0-23] sum of kWh
  var grid=[];var bucket=[];for(var d=0;d<7;d++){grid.push([]);bucket.push([]);for(var h=0;h<24;h++){grid[d].push(0);bucket[d].push([])}}
  var max=0;
  sessions.forEach(function(s){
    if(!s.ts||!s.en)return;
    var totalKwh=s.en/1000;
    var dur=s.dur||3600;
    var step=300;
    var n=Math.max(1,Math.ceil(dur/step));
    var kwhPerStep=totalKwh/n;
    for(var i=0;i<n;i++){
      var t=s.ts+i*step;
      if(t>=s.ts+dur)break;
      var dt=new Date(t*1000);
      var day,hr;
      try{var local=new Date(dt.toLocaleString('en-US',{timeZone:CHARGER_TZ}));day=local.getDay();hr=local.getHours()}catch(e){day=dt.getDay();hr=dt.getHours()}
      grid[day][hr]+=kwhPerStep;
      if(grid[day][hr]>max)max=grid[day][hr];
      var b=bucket[day][hr];if(b.indexOf(s)<0)b.push(s);
    }
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
      hm.innerHTML+="<div onclick='showHour("+d+","+h+")' title='"+DAYS[d]+" "+h+":00 — "+v.toFixed(1)+" kWh' style='aspect-ratio:1;background:"+bg+";border-radius:2px;cursor:"+(bucket[d][h].length?'pointer':'default')+"'></div>";
    }
  }
  window._heatmapBuckets=bucket;
}
// ---- Charging cost engine ----
var TARIFF=null;
function loadTariff(){try{TARIFF=JSON.parse(localStorage.getItem('wb-tariff')||'null')}catch(e){TARIFF=null}}
function saveTariff(t){try{localStorage.setItem('wb-tariff',JSON.stringify(t));TARIFF=t}catch(e){}}
function defaultTariff(){return{enabled:false,currency:'$',baseRate:0.30,greenRate:0,tiers:[]}}
function getRateAt(ts){
  if(!TARIFF||!TARIFF.enabled)return null;
  var d=new Date(ts*1000);
  var hr=0,day=0;
  try{var local=new Date(d.toLocaleString('en-US',{timeZone:CHARGER_TZ}));hr=local.getHours();day=local.getDay()}catch(e){hr=d.getHours();day=d.getDay()}
  // JS getDay: 0=Sun..6=Sat. Our bitmask: bit0=Mon..bit6=Sun. Convert.
  var bit=(day===0)?6:(day-1);
  for(var i=0;i<(TARIFF.tiers||[]).length;i++){
    var t=TARIFF.tiers[i];
    if(!(t.days&(1<<bit)))continue;
    var inRange=(t.fromH<=t.toH)?(hr>=t.fromH&&hr<t.toH):(hr>=t.fromH||hr<t.toH);
    if(inRange)return t.rate;
  }
  return TARIFF.baseRate;
}
function savingsOf(session){
  if(!TARIFF||!TARIFF.enabled)return null;
  if(!session.ts||!session.gen)return 0;
  var greenKwh=(session.gen||0)/1000;
  var totalKwh=(session.en||0)/1000;
  if(greenKwh>totalKwh)greenKwh=totalKwh;
  var step=300;
  var dur=session.dur||3600;
  var n=Math.max(1,Math.ceil(dur/step));
  var sumRate=0,count=0;
  for(var i=0;i<n;i++){
    var t=session.ts+i*step;
    if(t>=session.ts+dur)break;
    var r=getRateAt(t);
    if(r==null)r=TARIFF.baseRate;
    sumRate+=r;count++;
  }
  var avgRate=count>0?sumRate/count:TARIFF.baseRate;
  return greenKwh*Math.max(0,avgRate-TARIFF.greenRate);
}
function costOf(session){
  if(!TARIFF||!TARIFF.enabled)return null;
  if(!session.ts||!session.en)return 0;
  var totalKwh=session.en/1000;
  var greenKwh=(session.gen||0)/1000;
  if(greenKwh>totalKwh)greenKwh=totalKwh;
  var gridKwh=totalKwh-greenKwh;
  var dur=session.dur||3600;
  var step=300;
  var n=Math.max(1,Math.ceil(dur/step));
  var gridPerStep=gridKwh/n;
  var cost=greenKwh*TARIFF.greenRate;
  for(var i=0;i<n;i++){
    var t=session.ts+i*step;
    if(t>=session.ts+dur)break;
    var rate=getRateAt(t);
    if(rate==null)rate=TARIFF.baseRate;
    cost+=gridPerStep*rate;
  }
  return cost;
}
function fmt$(amt){if(amt==null||isNaN(amt))return'-';var sym=(TARIFF&&TARIFF.currency)||'$';return sym+amt.toFixed(2)}
loadTariff();

function renderTiles(sessions, lifetimeKwh){
  var nowSec=Date.now()/1000;
  var weekSec=nowSec-7*86400;
  var monthSec=nowSec-30*86400;
  var weekKwh=0,monthKwh=0;
  sessions.forEach(function(s){
    if(!s.ts||!s.en)return;
    var kwh=s.en/1000;
    if(s.ts>=weekSec)weekKwh+=kwh;
    if(s.ts>=monthSec)monthKwh+=kwh;
  });
  if(lifetimeKwh!=null)document.getElementById('tile-allt').textContent=lifetimeKwh.toFixed(0)+' kWh';
  document.getElementById('tile-week').textContent=weekKwh.toFixed(1)+' kWh';
  document.getElementById('tile-month').textContent=monthKwh.toFixed(1)+' kWh';
  // Cost tiles (computed if tariff enabled)
  if(TARIFF&&TARIFF.enabled){
    var weekCost=0,monthCost=0,weekSaved=0,monthSaved=0;
    sessions.forEach(function(s){
      if(!s.ts)return;
      var c=costOf(s);if(c!=null){if(s.ts>=weekSec)weekCost+=c;if(s.ts>=monthSec)monthCost+=c}
      var sv=savingsOf(s);if(sv!=null){if(s.ts>=weekSec)weekSaved+=sv;if(s.ts>=monthSec)monthSaved+=sv}
    });
    var costRow=document.getElementById('cost-row');if(costRow)costRow.style.display='grid';
    var w=document.getElementById('tile-week-cost');if(w)w.textContent=fmt$(weekCost);
    var m=document.getElementById('tile-month-cost');if(m)m.textContent=fmt$(monthCost);
    var ws=document.getElementById('tile-week-saved');if(ws)ws.textContent=weekSaved>0.005?'☀ saved '+fmt$(weekSaved):'';
    var ms=document.getElementById('tile-month-saved');if(ms)ms.textContent=monthSaved>0.005?'☀ saved '+fmt$(monthSaved):'';
  }else{
    var costRow=document.getElementById('cost-row');if(costRow)costRow.style.display='none';
  }
}
function fmtTime(ts){var d=new Date(ts*1000);try{return d.toLocaleTimeString(undefined,{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){return d.toLocaleTimeString()}}
function fmtDur(sec){var h=Math.floor(sec/3600),m=Math.round((sec%3600)/60);return (h?h+'h ':'')+m+'m'}
function toggleDay(i){var det=document.getElementById('det-'+i);var ch=document.getElementById('ch-'+i);if(!det)return;if(det.style.display==='none'){det.style.display='block';ch.textContent='\u25B4'}else{det.style.display='none';ch.textContent='\u25BE'}}
function renderDays(sessions){
  var byDay={};
  sessions.forEach(function(s){
    if(!s.ts||!s.en)return;
    var d=new Date(s.ts*1000);
    var key;
    try{key=d.toLocaleDateString('en-CA',{timeZone:CHARGER_TZ})}catch(e){key=d.toLocaleDateString('en-CA')}
    if(!byDay[key])byDay[key]={kwh:0,sec:0,n:0,first:s.ts,last:s.ts+(s.dur||0),items:[]};
    byDay[key].kwh+=(s.en||0)/1000;
    byDay[key].sec+=(s.dur||0);
    byDay[key].n++;
    byDay[key].items.push(s);
    if(s.ts<byDay[key].first)byDay[key].first=s.ts;
    var endTs=s.ts+(s.dur||0);
    if(endTs>byDay[key].last)byDay[key].last=endTs;
  });
  var dayKeys=Object.keys(byDay).sort().reverse().slice(0,10);
  var html='';
  dayKeys.forEach(function(key,i){
    var d=byDay[key];
    var dd=new Date(key+'T12:00:00');
    var dayLabel;
    try{dayLabel=dd.toLocaleDateString(undefined,{timeZone:CHARGER_TZ,weekday:'short',month:'short',day:'numeric'})}catch(e){dayLabel=key}
    var startStr=fmtTime(d.first),endStr=fmtTime(d.last);
    var durStr=fmtDur(d.sec);
    var nLabel=d.n>1?' \u00B7 '+d.n+' sessions':'';
    var detail='';
    d.items.sort(function(a,b){return a.ts-b.ts}).forEach(function(s){
      var sStart=fmtTime(s.ts);
      var sEnd=s.dur?fmtTime(s.ts+s.dur):'?';
      var sDur=fmtDur(s.dur||0);
      var sKwh=((s.en||0)/1000).toFixed(2);
      detail+="<div style='display:flex;justify-content:space-between;padding:6px 4px;border-top:1px solid var(--border);font-size:.8em'><div><div>"+sStart+" \u2013 "+sEnd+"</div><div style='color:var(--text3);font-size:.92em;margin-top:2px'>#"+s.id+" \u00B7 "+sDur+"</div></div><div style='text-align:right'><div style='font-weight:500'>"+sKwh+" kWh</div>"+(TARIFF&&TARIFF.enabled?"<div style='font-size:.75em;color:#22c55e'>"+fmt$(costOf(s))+"</div>":"")+"</div></div>";
    });
    html+="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:4px 0;cursor:pointer;user-select:none' onclick='toggleDay("+i+")'>"+
      "<div style='display:flex;justify-content:space-between;align-items:center'>"+
        "<div><div style='font-size:.88em;font-weight:500'>"+dayLabel+" <span id='ch-"+i+"' style='color:var(--text3);font-size:.85em'>\u25BE</span></div>"+
        "<div style='font-size:.78em;color:var(--text3);margin-top:2px'>"+startStr+" \u2013 "+endStr+nLabel+" \u00B7 "+durStr+"</div></div>"+
        "<div style='text-align:right'><div style='font-weight:600;font-size:1.05em'>"+d.kwh.toFixed(2)+" kWh</div>"+(TARIFF&&TARIFF.enabled?"<div style='font-size:.78em;color:#22c55e;margin-top:2px'>"+fmt$(d.items.reduce(function(a,s){return a+(costOf(s)||0)},0))+"</div>":"")+"</div>"+
      "</div>"+
      "<div id='det-"+i+"' style='display:none;margin-top:6px'>"+detail+"</div>"+
    "</div>";
  });
  var ids=sessions.map(function(s){return s.id||0}).filter(function(n){return n>0});
  var minId=ids.length?Math.min.apply(null,ids):0;
  if(minId>1){html+="<div style='text-align:center;margin-top:10px'><button id='load-older-btn' class='btn btn-outline' onclick='loadOlder()'>Load older sessions</button></div>";}
  document.getElementById('slist').innerHTML=html||'<span style="color:var(--text3)">No data yet</span>';
}
function loadOlder(){
  var cache;try{cache=JSON.parse(localStorage.getItem('wb-sessions-v1')||'{}')}catch(e){cache={}}
  if(!cache.s)cache.s={};
  var sids=Object.keys(cache.s).map(Number);
  var minCached=sids.length?Math.min.apply(null,sids):0;
  if(minCached<=1){toast('No older sessions','info');return}
  var startSid=Math.max(1,minCached-60),endSid=minCached-1;
  var total=endSid-startSid+1,fetched=0,sid=startSid;
  var btn=document.getElementById('load-older-btn');
  function fetchNext(){
    if(sid>endSid){try{localStorage.setItem('wb-sessions-v1',JSON.stringify(cache))}catch(e){}
      var allList=Object.keys(cache.s).map(function(k){return cache.s[k]});
      renderAll(allList,cache.lifetimeKwh);return}
    if(btn){btn.textContent='Loading '+(fetched+1)+' / '+total+'...';btn.disabled=true}
    var advanced=false;
    var advance=function(){if(advanced)return;advanced=true;sid++;fetched++;fetchNext()};
    var hardTimer=setTimeout(advance,9000);
    var thisSid=sid;
    fetch('/api/command?action=bapi&met=r_log&par='+thisSid,{signal:AbortSignal.timeout(8000)})
      .then(function(x){return x.json()}).then(function(sd){
        clearTimeout(hardTimer);
        if(sd.r&&sd.r.start){cache.s[thisSid]={id:thisSid,ts:sd.r.start,dur:sd.r.sec||sd.r.dur||sd.r.duration||0,en:sd.r.en||sd.r.energy||0,gen:sd.r.gen||0}}
        advance();
      }).catch(function(){clearTimeout(hardTimer);advance()});
  }
  fetchNext();
}
function renderAll(sessions, lifetimeKwh){
  renderTiles(sessions, lifetimeKwh);
  renderDays(sessions);
  try{buildHeatmap(sessions)}catch(e){console.error('heatmap failed',e)}
}
function closeHeatmapModal(){var m=document.getElementById('heatmap-modal');if(m)m.remove()}
function showHour(day,hr){
  var b=(window._heatmapBuckets&&window._heatmapBuckets[day]&&window._heatmapBuckets[day][hr])||[];
  if(!b.length)return;
  b=b.slice().sort(function(a,c){return (c.ts||0)-(a.ts||0)});
  var modal=document.createElement('div');
  modal.id='heatmap-modal';
  modal.style.cssText='position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.55);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);z-index:300;display:flex;align-items:center;justify-content:center;padding:16px';
  modal.onclick=function(e){if(e.target===modal)closeHeatmapModal()};
  var inner="<div style='background:var(--card);border-radius:12px;max-width:480px;width:100%;max-height:80vh;overflow:auto;padding:16px'><div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px'><h3 style='margin:0'>"+DAYS[day]+" "+hr+":00 \u2013 "+(hr+1)+":00</h3><button onclick='closeHeatmapModal()' style='background:transparent;border:none;color:var(--text2);font-size:1.6em;cursor:pointer;line-height:1'>\u00D7</button></div>";
  inner+="<div style='font-size:.78em;color:var(--text3);margin-bottom:8px'>"+b.length+" session"+(b.length===1?'':'s')+" delivered energy in this hour</div>";
  b.forEach(function(s){
    var d=new Date(s.ts*1000);
    var dateStr,startStr,endStr;
    try{dateStr=d.toLocaleDateString(undefined,{timeZone:CHARGER_TZ,weekday:'short',month:'short',day:'numeric'})}catch(e){dateStr=d.toDateString()}
    try{startStr=d.toLocaleTimeString(undefined,{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){startStr=d.toLocaleTimeString()}
    var endD=s.dur?new Date((s.ts+s.dur)*1000):null;
    if(endD)try{endStr=endD.toLocaleTimeString(undefined,{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){endStr=endD.toLocaleTimeString()}
    var dur=s.dur?Math.floor(s.dur/3600)+'h '+Math.round((s.dur%3600)/60)+'m':'-';
    var kwh=s.en?(s.en/1000).toFixed(2)+' kWh':'-';
    inner+="<div style='background:var(--bg);border-radius:8px;padding:10px;margin:6px 0'><div style='display:flex;justify-content:space-between;align-items:center'><div><div style='font-weight:500;font-size:.9em'>"+dateStr+"</div><div style='font-size:.78em;color:var(--text3);margin-top:2px'>"+startStr+(endD?' \u2013 '+endStr:'')+" \u00B7 "+dur+"</div></div><div style='font-weight:600'>"+kwh+"</div></div></div>";
  });
  inner+="</div>";
  modal.innerHTML=inner;
  document.body.appendChild(modal);
}
function exportCsv(){
  var cache;try{cache=JSON.parse(localStorage.getItem('wb-sessions-v1')||'{}')}catch(e){cache={}}
  var arr=Object.keys(cache.s||{}).map(function(k){return cache.s[k]});
  if(!arr.length){toast('No sessions to export','info');return}
  arr.sort(function(a,c){return (a.ts||0)-(c.ts||0)});
  var rows=[['Session ID','Date','Start','End','Duration (min)','Energy (kWh)','Cost']];
  arr.forEach(function(s){
    if(!s.ts)return;
    var sd=new Date(s.ts*1000);
    var ed=s.dur?new Date((s.ts+s.dur)*1000):null;
    var dateStr,startStr,endStr='';
    try{dateStr=sd.toLocaleDateString('en-CA',{timeZone:CHARGER_TZ})}catch(e){dateStr=sd.toLocaleDateString('en-CA')}
    try{startStr=sd.toLocaleTimeString(undefined,{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){startStr=sd.toLocaleTimeString()}
    if(ed)try{endStr=ed.toLocaleTimeString(undefined,{timeZone:CHARGER_TZ,hour:'2-digit',minute:'2-digit',hour12:false})}catch(e){endStr=ed.toLocaleTimeString()}
    var dur=s.dur?Math.round(s.dur/60):'';
    var kwh=s.en?(s.en/1000).toFixed(2):'';
    rows.push([s.id,dateStr,startStr,endStr,dur,kwh,(TARIFF&&TARIFF.enabled)?costOf(s).toFixed(2):'']);
  });
  var csv=rows.map(function(r){return r.join(',')}).join('\n');
  var blob=new Blob([csv],{type:'text/csv'});
  var url=URL.createObjectURL(blob);
  var a=document.createElement('a');a.href=url;a.download='wallbox-sessions-'+new Date().toISOString().slice(0,10)+'.csv';
  document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);
  toast('Downloaded '+(rows.length-1)+' sessions','success');
}
function loadSessions2(){
  var cache;
  try{cache=JSON.parse(localStorage.getItem('wb-sessions-v1')||'{}')}catch(e){cache={}}
  if(!cache.s)cache.s={};
  var cachedList=Object.keys(cache.s).map(function(k){return cache.s[k]});
  if(cachedList.length){renderAll(cachedList,cache.lifetimeKwh)}
  fetch('/api/command?action=bapi&met=r_dca&par=null',{signal:AbortSignal.timeout(10000)})
    .then(function(x){return x.json()}).then(function(d){
      if(d.r&&d.r.e){
        cache.lifetimeKwh=d.r.e/1000;
        document.getElementById('tile-allt').textContent=cache.lifetimeKwh.toFixed(0)+' kWh';
      }
    }).catch(function(){});
  fetch('/api/command?action=bapi&met=r_ses&par=null',{signal:AbortSignal.timeout(15000)})
    .then(function(x){return x.json()}).then(function(d){
      if(!d.r||!d.r.last){if(!cachedList.length)document.getElementById('slist').innerHTML='No sessions yet';return}
      var last=d.r.last;
      var cachedSids=Object.keys(cache.s).map(Number);
      var maxCached=cachedSids.length?Math.max.apply(null,cachedSids):0;
      if(maxCached>=last){try{localStorage.setItem('wb-sessions-v1',JSON.stringify(cache))}catch(e){}return}
      var startSid=maxCached===0?Math.max(1,last-59):maxCached+1;
      var total=last-startSid+1;
      var fetched=0,sid=startSid;
      function fetchNext(){
        if(sid>last){
          var allList=Object.keys(cache.s).map(function(k){return cache.s[k]});
          try{localStorage.setItem('wb-sessions-v1',JSON.stringify(cache))}catch(e){}
          renderAll(allList,cache.lifetimeKwh);
          return;
        }
        if(!cachedList.length){
          document.getElementById('slist').innerHTML="<span class='spinner'></span> Loading "+(fetched+1)+" / "+total+"...";
        }
        var advanced=false;
        var advance=function(){if(advanced)return;advanced=true;sid++;fetched++;fetchNext()};
        var hardTimer=setTimeout(advance,9000);
        var thisSid=sid;
        fetch('/api/command?action=bapi&met=r_log&par='+thisSid,{signal:AbortSignal.timeout(8000)})
          .then(function(x){return x.json()}).then(function(sd){
            clearTimeout(hardTimer);
            if(sd.r&&sd.r.start){
              cache.s[thisSid]={id:thisSid,ts:sd.r.start,dur:sd.r.sec||sd.r.dur||sd.r.duration||0,en:sd.r.en||sd.r.energy||0,gen:sd.r.gen||0};
            }
            advance();
          }).catch(function(){clearTimeout(hardTimer);advance()});
      }
      fetchNext();
    }).catch(function(e){
      if(!cachedList.length){document.getElementById('slist').innerHTML='<span style="color:var(--danger)">'+(e.message||e)+'</span>'}
    });
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
// Single retry budget — admission rejections (just-rebooted, BLE still
// settling, WiFi briefly down) are usually transient. Retrying more than
// once invites flash-storm behaviour we expressly designed the admission
// guard to prevent.
var _otaRetried=false;
function doOTA(){
  var f=document.getElementById('fw').files[0];
  if(!f){toast('Select a firmware file','error');return}
  if(!f.name.endsWith('.bin')){toast('Must be a .bin file','error');return}
  if(f.size<10000||f.size>2000000){toast('File size invalid','error');return}
  _otaRetried=false;
  _doOtaUpload(f);
}
function _doOtaUpload(f){
  var prog=document.getElementById('ota-progress');prog.style.display='block';
  var bar=document.getElementById('ota-bar');
  var stat=document.getElementById('ota-status');
  var xhr=new XMLHttpRequest();xhr.open('POST','/api/ota');
  xhr.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round(e.loaded/e.total*100);bar.style.width=pct+'%';stat.textContent='Uploading... '+pct+'%'}};
  xhr.onload=function(){
    if(xhr.status===200){
      stat.textContent='Update complete! Rebooting...';
      toast('Firmware updated!','success');
      bar.style.width='100%';bar.style.background='var(--success)';
      return;
    }
    if(xhr.status===503&&!_otaRetried){
      // Admission rejected — the gateway is asking us to back off and
      // try again shortly. Parse Retry-After (seconds) and schedule one
      // automatic retry. Avoids the user having to manually re-click
      // Upload after a reboot when the device hasn't quite passed the
      // settling window yet.
      var ra=parseInt(xhr.getResponseHeader('Retry-After')||'',10);
      if(!(ra>0)){
        try{var j=JSON.parse(xhr.responseText||'{}');if(j&&j.retry_after>0)ra=j.retry_after}catch(e){}
      }
      if(!(ra>0))ra=10;
      _otaRetried=true;
      bar.style.width='0%';bar.style.background='var(--accent)';
      var reason='';try{var j2=JSON.parse(xhr.responseText||'{}');reason=j2&&j2.error?j2.error:''}catch(e){}
      var remain=ra;
      stat.textContent='Gateway busy ('+(reason||'admission')+ ') — retrying in '+remain+'s...';
      toast('Gateway busy, auto-retrying in '+remain+'s','info');
      var timer=setInterval(function(){
        remain--;
        if(remain>0){stat.textContent='Gateway busy — retrying in '+remain+'s...';return}
        clearInterval(timer);
        stat.textContent='Retrying upload...';
        // Guard against synchronous throws inside _doOtaUpload (FormData
        // / XHR constructor failures on exotic browsers). The interval is
        // already cleared above, but without this catch the user is left
        // staring at "Retrying upload..." with no feedback if it throws.
        try{_doOtaUpload(f)}catch(e){
          stat.textContent='Retry failed: '+(e&&e.message?e.message:e);
          bar.style.background='var(--danger)';
          toast('Retry failed','error');
        }
      },1000);
      return;
    }
    stat.textContent='Failed: '+xhr.responseText;
    toast('Update failed','error');
    bar.style.background='var(--danger)';
  };
  xhr.onerror=function(){stat.textContent='Upload failed';toast('Upload error','error')};
  // CRITICAL: wrap the file in FormData so the body is sent as
  // multipart/form-data. xhr.send(File) by itself sends the raw bytes
  // with Content-Type set to the file's MIME (octet-stream for .bin),
  // and the Arduino WebServer routes those through the raw() dispatch
  // path which never allocates _currentUpload — handleOtaUpload() then
  // dereferences a null unique_ptr → LoadProhibited panic.
  // peter-mcc #4: this is what bricked every browser-driven OTA on
  // his board until decoded from the serial backtrace.
  var fd=new FormData();fd.append('firmware',f);
  xhr.send(fd);
}
document.getElementById('ld').style.display='none';document.getElementById('pg').style.display='block';
</script>
)HTML";
    page += htmlFoot("/info");
    http.send(200, "text/html", page);
}

bool otaInProgress = false;
static size_t expectedOtaSize = 0;  // Content-Length captured at FILE_START for truncation check
// When admission rejects an upload at FILE_START we set otaRetryAfterSec so
// FILE_END can emit a proper 503 + Retry-After. doOTA() in the browser
// parses Retry-After and schedules one auto-retry — peter-mcc #4 follow-up:
// the typical "rejected because we just rebooted" case should self-heal
// without the user re-clicking Upload.
static uint16_t otaRetryAfterSec = 0;
static String   otaRejectReason;

static void handleOtaUpload() {
    // DEFENCE IN DEPTH — the Arduino WebServer routes both multipart AND
    // raw POST bodies through this same lambda (FunctionRequestHandler
    // calls _ufn() from both upload() and raw() dispatch). _currentUpload
    // is ONLY allocated for multipart; for raw bodies http.upload() returns
    // *_currentUpload which is a null unique_ptr → LoadProhibited panic on
    // dereference. peter-mcc #4 hit this because xhr.send(File) without
    // FormData sends application/octet-stream, not multipart/form-data.
    //
    // The client-side fix (wrap in FormData) is in doOTA() above. This
    // is the server-side safety net for any other tool that POSTs raw
    // — HA, curl --data-binary, custom OTA scripts — so they get a
    // clean error instead of bricking the gateway.
    // Default to REJECT if Content-Type is missing or doesn't say
    // multipart. The previous version "if header present, check" was
    // wrong — when WebServer doesn't collect a header it just returns
    // empty, so the guard fired open on every raw POST and we still
    // crashed. Now the only path that reaches http.upload() is one
    // that explicitly says multipart/.
    String ct = http.header("Content-Type");
    if (ct.indexOf("multipart/") < 0) {
        Log.printf("[OTA] REJECTED: Content-Type='%s' (need multipart/form-data, raw body crashes handler)\n",
                   ct.c_str());
        return;  // body is consumed by the WebServer; we just don't act on it
    }

    HTTPUpload& upload = http.upload();
    static size_t totalSize = 0;
    static bool otaError = false;

    if (upload.status == UPLOAD_FILE_START) {
        // Clear any retry hint left over from a prior aborted upload —
        // FILE_END below only emits 503+Retry-After if THIS upload sets it.
        otaRetryAfterSec = 0;
        otaRejectReason  = String();
        // SECURITY — auth check must happen BEFORE the admission guard,
        // BEFORE Update.begin() erases the partition. Without this anyone
        // on the WiFi can flash arbitrary firmware and brick or backdoor
        // the gateway. checkAuth() is a no-op when web auth isn't
        // enabled (matches the rest of the API surface).
        if (!checkAuth()) {
            Log.println("[OTA] REJECTED: unauthenticated");
            otaError = true;
            return;
        }
        // Admission guard — reject if the gateway hasn't been healthy long
        // enough or another OTA is in progress. Prevents flash-storms
        // (rapid back-to-back OTAs colliding with the post-reboot
        // settling window) and re-entrant uploads.
        if (otaInProgress) {
            Log.println("[OTA] REJECTED: another OTA already in progress");
            otaError          = true;
            otaRetryAfterSec  = 10;  // the other OTA should be done by then
            otaRejectReason   = "another OTA already in progress";
            return;
        }
        String reason;
        if (!wb_health::canAcceptOta(reason)) {
            Log.printf("[OTA] REJECTED: %s\n", reason.c_str());
            otaError         = true;
            otaRetryAfterSec = (uint16_t)wb_health::otaRetryAfterSeconds();
            otaRejectReason  = reason;
            return;
        }

        Log.printf("[OTA] Upload start: %s\n", upload.filename.c_str());
        totalSize = 0;
        otaError = false;
        otaInProgress = true;

        // Pause BLE for the OTA window. BLE scans/reconnects set the radio
        // coex preference to BT and starve WiFi for several seconds at a
        // time — bad for a streaming OTA TCP upload.
        wallboxBLE.pause(5 * 60 * 1000);  // 5 min — auto-resumes after

        // Extend the Task Watchdog timeout to cover the blocking flash
        // erase that's about to happen inside Update.begin(). On an empty
        // / fully-used OTA partition the erase can take 10+ seconds, well
        // beyond the default 5s WDT — which would otherwise panic-reboot
        // mid-upload (reported by peter-mcc in #4). Restored on FILE_END
        // and ABORTED so a failed OTA doesn't leave the WDT permanently
        // relaxed — see wb_watchdog.h.
        wb_wdt::extendTo(60);

        // Use the HTTP Content-Length to size Update.begin() — this
        // erases only as much as needed instead of the whole 1.9 MB
        // partition. Browser-side time-to-first-byte is much shorter.
        size_t expected = 0;
        if (http.hasHeader("Content-Length")) {
            expected = (size_t) http.header("Content-Length").toInt();
            // Content-Length includes multipart envelope (~150 bytes); fine to over-erase by that much
        }
        const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
        if (partition) {
            Log.printf("[OTA] Target partition: %s (%u bytes), expected upload: %u\n",
                       partition->label, partition->size, (unsigned)expected);
        }
        // Sanity: Content-Length must fit the OTA partition with some margin.
        // Refusing here means we never erase if the upload is clearly bogus.
        if (partition && expected > 0 && expected > partition->size) {
            Log.printf("[OTA] REJECTED: payload (%u) larger than partition (%u)\n",
                       (unsigned)expected, (unsigned)partition->size);
            otaError = true;
            otaInProgress = false;
            return;
        }
        expectedOtaSize = expected;

        bool ok = expected > 0 ? Update.begin(expected) : Update.begin(UPDATE_SIZE_UNKNOWN);
        if (!ok) {
            Log.printf("[OTA] Begin failed: %s\n", Update.errorString());
            otaError = true;
        } else if (http.hasHeader("X-Firmware-MD5")) {
            // Optional end-to-end integrity check. When supplied, the
            // Update library streams an MD5 alongside the partition
            // writes and fails Update.end() if the final hash doesn't
            // match — so a flipped byte mid-flight (rare on TCP, but
            // possible on Wi-Fi with marginal signal) gets caught
            // before we mark the partition valid and reboot into it.
            String md5 = http.header("X-Firmware-MD5");
            md5.trim();
            md5.toLowerCase();
            // 32 hex chars = 16 bytes. Anything else is malformed —
            // skip rather than fail so a typo on the client side doesn't
            // brick the upload outright.
            if (md5.length() == 32) {
                if (Update.setMD5(md5.c_str())) {
                    Log.printf("[OTA] Expecting MD5 %s\n", md5.c_str());
                } else {
                    Log.println("[OTA] WARN: Update.setMD5() rejected — proceeding without integrity check");
                }
            } else {
                Log.printf("[OTA] WARN: X-Firmware-MD5 has wrong length (%u, expected 32) — ignored\n",
                           (unsigned)md5.length());
            }
        }
    } else if (upload.status == UPLOAD_FILE_WRITE && !otaError) {
        // First chunk — validate ESP32 magic byte
        if (totalSize == 0 && upload.currentSize > 0) {
            if (upload.buf[0] != 0xE9) {
                Log.println("[OTA] REJECTED: not ESP32 firmware (magic byte != 0xE9)");
                Update.abort();
                otaError = true;
                return;
            }
            Log.println("[OTA] Firmware magic byte OK");
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Log.printf("[OTA] Write failed: %s\n", Update.errorString());
            otaError = true;
        }
        totalSize += upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        otaInProgress = false;
        // Restore the WDT regardless of success or error. Doing it here
        // catches every terminal path below — success branch reboots
        // anyway, but error paths must not leave the WDT relaxed.
        wb_wdt::restore();
        // Truncation check — if we received fewer bytes than Content-Length
        // promised, refuse to commit. Otherwise `Update.end(true)` will
        // happily mark a partial OTA partition as valid and brick the device.
        // Tolerance: ±256 bytes for the multipart envelope.
        if (!otaError && expectedOtaSize > 0) {
            size_t diff = (totalSize < expectedOtaSize)
                            ? expectedOtaSize - totalSize
                            : totalSize - expectedOtaSize;
            if (diff > 256) {
                Log.printf("[OTA] TRUNCATED: expected ~%u bytes, got %u — aborting\n",
                    (unsigned)expectedOtaSize, (unsigned)totalSize);
                otaError = true;
            }
        }
        if (otaError) {
            Update.abort();
            // Admission-rejection path: emit a proper 503 with Retry-After
            // so the browser can auto-retry once the window opens. Other
            // errors (truncation, magic byte, Update.begin failure) get
            // 500 — those won't help by retrying.
            if (otaRetryAfterSec > 0) {
                Log.printf("[OTA] Rejected — telling client to retry in %us: %s\n",
                           (unsigned)otaRetryAfterSec, otaRejectReason.c_str());
                wb_ota_history::record(millis() / 1000, WB_VERSION, totalSize, false,
                                       (String("rejected: ") + otaRejectReason).c_str());
                http.sendHeader("Retry-After", String(otaRetryAfterSec));
                String body = "{\"error\":\"" + otaRejectReason
                            + "\",\"retry_after\":" + String(otaRetryAfterSec) + "}";
                http.send(503, "application/json", body);
            } else {
                Log.println("[OTA] Aborted due to errors");
                wb_ota_history::record(millis() / 1000, WB_VERSION, totalSize, false, "aborted");
                http.send(500, "text/plain", "Upload failed");
            }
        } else if (Update.end(true)) {
            Log.printf("[OTA] Success! %u bytes written to partition\n", totalSize);
            wb_ota_history::record(millis() / 1000, WB_VERSION, totalSize, true, "ok");
            // Mark this device as OTA-proven so future flashes use the
            // relaxed 15s admission window instead of the conservative
            // 60s one. Safe to call repeatedly — internally NVS-cached.
            wb_health::markOtaSuccess();
            http.send(200, "text/plain", "OK");
            delay(1000);
            ESP.restart();
        } else {
            Log.printf("[OTA] End failed: %s\n", Update.errorString());
            wb_ota_history::record(millis() / 1000, WB_VERSION, totalSize, false, Update.errorString());
            http.send(500, "text/plain", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        // Client (or our admission guard) bailed mid-flow. Make sure we
        // don't leave Update.begin()'s erased partition in a half-committed
        // state — abort it explicitly so the bootloader keeps booting the
        // current healthy partition.
        if (otaInProgress) {
            Update.abort();
            otaInProgress = false;
            wb_wdt::restore();
            Log.println("[OTA] Upload aborted by client — partition left untouched");
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
    // Capture Content-Length so the OTA handler can size Update.begin().
    // Capture Content-Type so the OTA handler can distinguish multipart
    // (good) from raw POST (would crash on http.upload() — see #4).
    // Arduino WebServer drops every header NOT in this list, so the
    // OTA handler's Content-Type guard depends on this entry existing.
    // X-Firmware-MD5 is honored by the OTA handler — when present, the
    // Arduino Update library computes the hash as bytes stream in and
    // refuses to commit on mismatch. Tools that can compute MD5 of the
    // .bin (curl --header, HA OTA scripts) get end-to-end integrity for
    // free; browsers don't send it today (kept off the upload page to
    // avoid the multi-second hash step on a 1.5 MB file).
    static const char* otaHeaders[] = {"Content-Length", "Content-Type", "X-Firmware-MD5"};
    http.collectHeaders(otaHeaders, 3);
    http.on("/style.css", handleStyleCss);
    http.on("/app.js", handleAppJs);
    http.on("/save", HTTP_POST, handleSave);
    http.on("/reset", HTTP_POST, handleReset);
    // Reboot without modifying NVS — useful for capturing a fresh boot
    // trace from /logs without losing any config. Auth + CSRF gated.
    http.on("/api/reboot", HTTP_POST, []() {
        if (!checkAuth()) return;
        if (!checkCsrf()) return;
        http.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
        webServer.requestReboot();
    });
    // /api/pin?pin=<digits>&csrf=... — local-only update of the
    // Bluetooth Passcode used to pair to the charger. Does NOT call
    // any s_* setter on the charger; the user creates the passcode in
    // the official Wallbox app and copies it here so our NimBLE pair
    // can authenticate. Empty pin clears the field. Reboots after
    // save so the new value takes effect on the next pair attempt.
    http.on("/api/pin", HTTP_POST, []() {
        if (!checkAuth()) return;
        if (!checkCsrf()) return;
        String pin = http.arg("pin");
        // Light sanity: trim, allow empty (clear), allow digits only
        // up to 16 chars. Reject anything else so we don't store
        // garbage that breaks the BLE pair flow.
        pin.trim();
        if (pin.length() > 16) {
            http.send(400, "application/json", "{\"error\":\"passcode too long\"}");
            return;
        }
        for (size_t i = 0; i < pin.length(); i++) {
            char c = pin[i];
            if (c < '0' || c > '9') {
                http.send(400, "application/json", "{\"error\":\"passcode must be digits only\"}");
                return;
            }
        }
        configMgr.mut().blePin = pin;
        configMgr.save();
        http.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
        webServer.requestReboot();
    });
    http.on("/api/ble-scan", handleBleScan);
    http.on("/api/wifi-scan", handleWifiScan);
    http.on("/api/status", handleApiStatus);
    http.on("/api/ble/pause", handleBlePause);
    http.on("/api/charger", handleApiCharger);
    http.on("/api/command", handleApiCommand);
    http.on("/api/fw/dismiss", HTTP_POST, []() {
        if (!checkAuth()) return;
        wallboxBLE.dismissFirmwareChange();
        http.send(200, "application/json", "{\"ok\":true}");
    });
    // /api/config/export — returns the gateway's NVS config as JSON, with
    // passwords/PINs masked. Safe to share for support/backup.
    http.on("/api/config/export", []() {
        if (!checkAuth()) return;
        const WBConfig& c = configMgr.get();
        JsonDocument d;
        d["version"] = 1;
        d["exported_at"] = millis() / 1000;
        d["wifi_ssid"]   = c.wifiSSID;
        d["wifi_pass"]   = c.wifiPass.length() ? "***" : "";
        d["mqtt_host"]   = c.mqttHost;
        d["mqtt_port"]   = c.mqttPort;
        d["mqtt_user"]   = c.mqttUser;
        d["mqtt_pass"]   = c.mqttPass.length() ? "***" : "";
        d["mqtt_cid"]    = c.mqttClientId;
        d["ble_addr"]    = c.bleAddr;
        d["ble_pin"]     = c.blePin.length() ? "***" : "";
        d["ble_svc"]     = c.bleService;
        d["ble_chr"]     = c.bleChar;
        d["ble_txchr"]   = c.bleTxChar;
        d["chg_model"]   = c.chargerModel;
        d["auth_enabled"]= c.authEnabled;
        d["auth_user"]   = c.authUser;
        d["auth_pass"]   = c.authPass.length() ? "***" : "";
        d["poll_status"] = c.statusPollMs;
        d["poll_rt"]     = c.realtimePollMs;
        d["ha_prefix"]   = c.haDiscoveryPrefix;
        d["ha_devid"]    = c.haDeviceId;
        String body;
        serializeJsonPretty(d, body);
        http.sendHeader("Content-Disposition", "attachment; filename=\"wallbox-config.json\"");
        http.send(200, "application/json", body);
    });

    // /api/config/import — POST a JSON payload to restore config. Values
    // equal to "***" are skipped (preserves the existing secret). The
    // gateway reboots after a successful import.
    http.on("/api/config/import", HTTP_POST, []() {
        if (!checkAuth()) return;
        if (!checkCsrf()) return;
        if (!http.hasArg("plain")) {
            http.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
            return;
        }
        JsonDocument d;
        if (deserializeJson(d, http.arg("plain")) != DeserializationError::Ok) {
            http.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
            return;
        }
        auto take = [&](JsonVariantConst v, String& dst) {
            if (v.is<const char*>()) {
                const char* s = v.as<const char*>();
                if (strcmp(s, "***") != 0) dst = s;
            }
        };
        WBConfig& c = configMgr.mut();
        take(d["wifi_ssid"],   c.wifiSSID);
        take(d["wifi_pass"],   c.wifiPass);
        take(d["mqtt_host"],   c.mqttHost);
        if (d["mqtt_port"].is<uint16_t>()) c.mqttPort = d["mqtt_port"].as<uint16_t>();
        take(d["mqtt_user"],   c.mqttUser);
        take(d["mqtt_pass"],   c.mqttPass);
        take(d["mqtt_cid"],    c.mqttClientId);
        take(d["ble_addr"],    c.bleAddr);
        take(d["ble_pin"],     c.blePin);
        take(d["ble_svc"],     c.bleService);
        take(d["ble_chr"],     c.bleChar);
        take(d["ble_txchr"],   c.bleTxChar);
        take(d["chg_model"],   c.chargerModel);
        if (d["auth_enabled"].is<bool>()) c.authEnabled = d["auth_enabled"].as<bool>();
        take(d["auth_user"],   c.authUser);
        take(d["auth_pass"],   c.authPass);
        if (d["poll_status"].is<uint32_t>()) c.statusPollMs = d["poll_status"].as<uint32_t>();
        if (d["poll_rt"].is<uint32_t>())     c.realtimePollMs = d["poll_rt"].as<uint32_t>();
        take(d["ha_prefix"],   c.haDiscoveryPrefix);
        take(d["ha_devid"],    c.haDeviceId);
        configMgr.save();
        http.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
        webServer.requestReboot();
    });

    // /api/ota/history — the last few OTA attempts (newest first)
    http.on("/api/ota/history", []() {
        if (!checkAuth()) return;
        http.sendHeader("Cache-Control", "no-store");
        http.send(200, "application/json", wb_ota_history::toJson());
    });
    // /api/diag/runtime — heap + per-task stack monitoring. The last
    // values logged via this endpoint can hint at memory pressure that
    // preceded a panic (the panic itself doesn't tell us that).
    http.on("/api/diag/runtime", []() {
        if (!checkAuth()) return;
        String body = "{";
        body += "\"heap_free\":" + String(ESP.getFreeHeap());
        body += ",\"heap_min_ever\":" + String(ESP.getMinFreeHeap());
        body += ",\"heap_max_alloc\":" + String(ESP.getMaxAllocHeap());
        body += ",\"psram_free\":" + String(ESP.getFreePsram());
        body += ",\"main_stack_hwm\":" + String(uxTaskGetStackHighWaterMark(NULL));
        body += ",\"ble_stack_hwm\":";
        TaskHandle_t ble = xTaskGetHandle("wb_ble");
        body += ble ? String(uxTaskGetStackHighWaterMark(ble)) : String("null");
        body += ",\"loop_stack_hwm\":";
        TaskHandle_t arduinoLoop = xTaskGetHandle("loopTask");
        body += arduinoLoop ? String(uxTaskGetStackHighWaterMark(arduinoLoop)) : String("null");
        body += ",\"uptime_s\":" + String(millis() / 1000);
        body += "}";
        http.sendHeader("Cache-Control", "no-store");
        http.send(200, "application/json", body);
    });
    // /api/boot/history — last ~10 boot reasons (newest first).
    // Lets us see WHY the gateway rebooted (panic / WDT / power-on / etc).
    http.on("/api/boot/history", []() {
        if (!checkAuth()) return;
        http.sendHeader("Cache-Control", "no-store");
        // current_fw lets /info's badge count "bad boots on THIS firmware"
        // instead of including pre-upgrade dev-testing panics still in
        // the NVS ring. Entries without a `fw` field are treated as
        // "prior firmware" by the client.
        String body = "{\"current\":\"" + String(wb_health::currentBootReasonStr())
                    + "\",\"current_fw\":\"" + String(WB_VERSION)
                    + "\",\"history\":" + wb_health::bootHistoryJson() + "}";
        http.send(200, "application/json", body);
    });
    // /api/diag/disconnects — counters + recent BLE/MQTT reconnect events
    http.on("/api/diag/disconnects", []() {
        if (!checkAuth()) return;
        http.sendHeader("Cache-Control", "no-store");
        http.send(200, "application/json", wb_diag::toJson());
    });
    // Manual clear for the counters (e.g., after acknowledging an outage)
    http.on("/api/diag/clear", HTTP_POST, []() {
        if (!checkAuth()) return;
        if (!checkCsrf()) return;
        wb_diag::clear();
        http.send(200, "application/json", "{\"ok\":true}");
    });

    // /api/logs — last ~16 KB of Serial/telnet output in chronological order.
    // Plain text so it's trivially curlable; auth-gated when web auth is on.
    http.on("/api/logs", []() {
        if (!checkAuth()) return;
        String body;
        Log.copyBuffer(body);
        http.sendHeader("Cache-Control", "no-store");
        http.send(200, "text/plain; charset=utf-8", body);
    });
    // /logs — auto-refreshing viewer
    http.on("/logs", []() {
        if (!checkAuth()) return;
        String page = htmlHead("Logs");
        page += "<h1>&#x1F4DC; Gateway Log <span id='log-state' style='font-size:.55em;vertical-align:middle;margin-left:8px;padding:3px 8px;border-radius:10px;background:rgba(34,197,94,.15);color:#22c55e'>online</span></h1>";
        page += "<p style='color:var(--text3);font-size:.82em'>Last 16 KB of serial/telnet output. Auto-refreshes every 3s; scroll-locks to bottom unless you scroll up. Persists across page reloads, wiped on reboot.</p>";
        page += "<div style='margin-bottom:8px'>";
        page += "<button class='btn btn-outline' style='padding:6px 12px;font-size:.85em' onclick='copyLog()'>&#x1F4CB; Copy</button> ";
        page += "<a href='/api/logs' download='wallbox-log.txt' class='btn btn-outline' style='padding:6px 12px;font-size:.85em;text-decoration:none'>&#x2B07; Download</a> ";
        page += "<button class='btn btn-outline' style='padding:6px 12px;font-size:.85em' onclick='confirm2(\"Reboot the gateway? Config is preserved \\u2014 you can watch the boot trace appear here.\",function(){fetch(\"/api/reboot?csrf=" + csrfToken + "\",{method:\"POST\"}).then(function(){}).catch(function(){})})'>&#x21BB; Reboot &amp; capture boot trace</button>";
        page += "</div>";
        page += "<pre id='log' style='background:var(--bg);border-radius:8px;padding:10px;font-size:.78em;max-height:70vh;overflow:auto;white-space:pre-wrap;line-height:1.35'></pre>";
        page += "<script>(function(){"
                "var el=document.getElementById('log');"
                "var st=document.getElementById('log-state');"
                "var stick=true;var fails=0;"
                "function setState(label,color,bg){st.textContent=label;st.style.color=color;st.style.background=bg}"
                "function offline(){setState('offline — gateway rebooting?','#ef4444','rgba(239,68,68,.15)')}"
                "function online(){setState('online','#22c55e','rgba(34,197,94,.15)');fails=0}"
                "el.addEventListener('scroll',function(){"
                  "stick=(el.scrollHeight-el.scrollTop-el.clientHeight)<8;"
                "});"
                "window.copyLog=function(){var t=el.textContent||'';if(navigator.clipboard){navigator.clipboard.writeText(t).then(function(){toast&&toast('Copied','success')}).catch(function(){})}};"
                "function load(){fetch('/api/logs',{cache:'no-store'})"
                  ".then(function(r){return r.text()})"
                  ".then(function(t){online();el.textContent=t;if(stick)el.scrollTop=el.scrollHeight})"
                  ".catch(function(){fails++;if(fails>=2)offline()});}"
                "load();setInterval(load,3000);"
                "})();</script>";
        page += htmlFoot("/logs");
        http.send(200, "text/html", page);
    });
    // Health endpoint — returns 200 only when the gateway is in a stable,
    // healthy state. Used by OTA tooling to confirm the previous flash
    // actually worked before attempting another. Returns 503 with a JSON
    // reason otherwise.
    http.on("/api/health", []() {
        String reason;
        bool ok = wb_health::canAcceptOta(reason);
        // max_reentry is the proof field: it must stay 1. >1 means the web
        // server was pumped re-entrantly (the panic class of bug). tokens =
        // current rate-limit budget. Both are cheap to read and let a stress
        // harness assert correctness over Wi-Fi without a serial console.
        String diag = ",\"max_reentry\":" + String(g_webMaxReentry)
                    + ",\"tokens\":" + String((int)_tbTokens)
                    // loop_max_ms = longest gap between consecutive main
                    // loop() iterations since boot. Healthy: under
                    // ~200 ms in steady state. A wedge (peter-mcc #4)
                    // would show as multi-second values here.
                    + ",\"loop_max_ms\":" + String((uint32_t)g_loopMaxMs)
                    + ",\"heap_free\":" + String(ESP.getFreeHeap())
                    + ",\"uptime\":" + String(millis()/1000)
                    // ota_proven flips to true the first time an OTA
                    // commits + the new firmware reaches healthy state.
                    // ota_min_uptime is the threshold currently in force
                    // (60s fresh, 15s once proven). Lets a tester confirm
                    // the relaxed window engaged without scraping logs.
                    + ",\"ota_proven\":" + String(wb_health::otaProven() ? "true" : "false")
                    + ",\"ota_min_uptime\":" + String(wb_health::effectiveOtaMinUptimeMs()/1000);
        if (ok) {
            http.send(200, "application/json", "{\"ok\":true" + diag + "}");
        } else {
            http.send(503, "application/json",
                "{\"ok\":false,\"reason\":\"" + reason + "\"" + diag + "}");
        }
    });
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
    Log.printf("[Web] AP: %s (pass: %s) IP: %s\n", AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    dns.start(53, "*", WiFi.softAPIP());

    registerRoutes();
    http.on("/", handleConfig);  // AP mode: config first
    http.on("/config", handleConfig);
    http.on("/dashboard", handleDashboard);
    http.on("/settings", handleSettings);
    http.on("/info", handleInfo);
    http.onNotFound(handleNotFound);
    http.begin();
    Log.println("[Web] AP captive portal ready");
}

void WBWebServer::beginSTA() {
    _apMode = false;
    registerRoutes();
    http.on("/", handleDashboard);  // STA mode: dashboard is home
    http.on("/config", handleConfig);
    http.on("/settings", handleSettings);
    http.on("/info", handleInfo);
    http.begin();
    Log.printf("[Web] http://%s/ (dashboard)\n", WiFi.localIP().toString().c_str());
}

void WBWebServer::loop() {
    if (_apMode) dns.processNextRequest();
    g_webReentryDepth++;
    if (g_webReentryDepth > g_webMaxReentry) g_webMaxReentry = g_webReentryDepth;
    http.handleClient();
    g_webReentryDepth--;
    if (_rebootRequested) {
        static uint32_t rt = 0;
        if (rt == 0) rt = millis();
        if (millis() - rt > 2000) { Log.println("[Web] Rebooting..."); ESP.restart(); }
    }
}
