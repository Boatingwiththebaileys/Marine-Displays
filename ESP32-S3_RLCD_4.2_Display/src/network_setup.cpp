/*
 * Network setup and web UI for ESP32-S3-RLCD-4.2 Marine Display.
 *
 * Ported from the Square Display project — stripped of color-display-specific
 * features (brightness, needles, background images, touch, RGB panel).
 * Provides: Home dashboard, Network/WiFi config, Device settings, OTA updates.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>
#include "network_setup.h"
#include "screen_config.h"
#include "screen_render.h"
#include "version.h"
#include "signalk_config.h"
#include "unit_convert.h"
#include "LVGL_Driver.h"
#include "audio_alert.h"

static const char *TAG_NET = "network_setup";

// ─── RST-close helper ──────────────────────────────────────────────────────
// Close HTTP client with RST so lwIP frees the PCB immediately (no TIME_WAIT).
static inline void rst_close_client() {
    WiFiClient cl = config_server.client();
    if (cl) {
        struct linger lg = { 1, 0 };
        setsockopt(cl.fd(), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        cl.stop();
    }
}

// ─── CSS ────────────────────────────────────────────────────────────────────
static const char STYLE[] PROGMEM = R"rawliteral(
<style>
body{font-family:Arial,Helvetica,sans-serif;background:#fff;color:#111}
.container{max-width:900px;margin:0 auto;padding:12px}
.tab-btn{background:#f4f6fa;border:1px solid #d8e0ef;border-radius:4px;padding:8px 12px;cursor:pointer}
.tab-content{border:1px solid #e6e9f2;padding:12px;border-radius:6px;background:#fff}
.status{background:#f1f7ff;border:1px solid #dbe8ff;padding:10px;border-radius:6px;margin-bottom:12px;color:#0b2f5a}
.root-actions{display:flex;justify-content:center;gap:12px;margin-top:8px;flex-wrap:wrap}
.form-row{display:flex;flex-direction:row;align-items:center;gap:8px;margin-bottom:10px}
.form-row label{width:140px;text-align:right;color:#0b3b6a}
input[type=text],input[type=password]{width:60%;padding:6px;border:1px solid #dfe9fb;border-radius:4px}
input[type=number]{width:120px;padding:6px;border:1px solid rgb(223, 233, 251);border-radius:4px}
</style>
)rawliteral";

// ─── Globals ────────────────────────────────────────────────────────────────

WebServer config_server(80);
static Preferences preferences;

static String saved_ssid = "";
static String saved_password = "";
static String saved_signalk_ip = "";
static uint16_t saved_signalk_port = 0;
static String saved_hostname = "";
static uint8_t saved_unit_system = 3;  // 0=Metric,1=Imp US,2=Imp UK,3=Naut Met,4=Naut US,5=Naut UK

uint16_t auto_scroll_sec = 0; // 0=off, otherwise seconds between screen changes
volatile unsigned long last_config_activity = 0;

static const char* SETTINGS_NAMESPACE = "settings";
static const char* SCREENS_NAMESPACE  = "screens";

// Screen configuration array
RlcdScreenConfig screen_configs[NUM_SCREENS];

// ─── Preferences persistence ───────────────────────────────────────────────

void save_preferences() {
    preferences.end();
    if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
        Serial.println("[ERROR] preferences.begin failed");
        return;
    }
    preferences.putString("ssid", saved_ssid);
    preferences.putString("password", saved_password);
    preferences.putString("signalk_ip", saved_signalk_ip);
    preferences.putUShort("signalk_port", saved_signalk_port);
    preferences.putString("hostname", saved_hostname);
    preferences.putUChar("unit_system", saved_unit_system);
    preferences.putUChar("buzzer_mode", buzzer_mode);
    preferences.putUChar("alert_vol", alert_volume);
    preferences.putUShort("buzzer_cd", buzzer_cooldown_sec);
    preferences.putUShort("auto_scroll", auto_scroll_sec);
    preferences.end();
    Serial.println("[NVS] Preferences saved");
}

static void save_screen_configs() {
    preferences.end();
    if (!preferences.begin(SCREENS_NAMESPACE, false)) {
        Serial.println("[ERROR] screens namespace begin failed");
        return;
    }
    for (int s = 0; s < NUM_SCREENS; s++) {
        char key[12];
        snprintf(key, sizeof(key), "scr%d", s);
        preferences.putBytes(key, &screen_configs[s], sizeof(RlcdScreenConfig));
    }
    preferences.end();
    Serial.println("[NVS] Screen configs saved");
}

void load_preferences() {
    preferences.end();
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
        saved_ssid = preferences.getString("ssid", "");
        saved_password = preferences.getString("password", "");
        saved_signalk_ip = preferences.getString("signalk_ip", "openplotter.local");
        saved_signalk_port = preferences.getUShort("signalk_port", 0);
        saved_hostname = preferences.getString("hostname", "");
        saved_unit_system = preferences.getUChar("unit_system", 3);
        unit_system = (UnitSystem)saved_unit_system;
        buzzer_mode = preferences.getUChar("buzzer_mode", 1);
        alert_volume   = preferences.getUChar("alert_vol", 80);
        buzzer_cooldown_sec = preferences.getUShort("buzzer_cd", 60);
        auto_scroll_sec = preferences.getUShort("auto_scroll", 0);
        preferences.end();
    }
    Serial.printf("[NVS] Loaded: ssid='%s' signalk_ip='%s' port=%u hostname='%s'\n",
                  saved_ssid.c_str(), saved_signalk_ip.c_str(), saved_signalk_port,
                  saved_hostname.c_str());

    // Load screen configs
    memset(screen_configs, 0, sizeof(screen_configs));
    // Defaults: all screens start as Number type, alerts disabled
    for (int s = 0; s < NUM_SCREENS; s++) {
        screen_configs[s].display_type = DISPLAY_TYPE_NUMBER;
        screen_configs[s].number_font_size = RLCD_FONT_96;
        screen_configs[s].dual_top_font_size = RLCD_FONT_72;
        screen_configs[s].dual_bottom_font_size = RLCD_FONT_72;
        screen_configs[s].graph_time_range = RLCD_GRAPH_5M;
        // All alert thresholds default to disabled
        screen_configs[s].number_alert_low = ALERT_OFF;
        screen_configs[s].number_alert_high = ALERT_OFF;
        screen_configs[s].dual_top_alert_low = ALERT_OFF;
        screen_configs[s].dual_top_alert_high = ALERT_OFF;
        screen_configs[s].dual_bot_alert_low = ALERT_OFF;
        screen_configs[s].dual_bot_alert_high = ALERT_OFF;
        screen_configs[s].quad_tl_alert_low = ALERT_OFF;
        screen_configs[s].quad_tl_alert_high = ALERT_OFF;
        screen_configs[s].quad_tr_alert_low = ALERT_OFF;
        screen_configs[s].quad_tr_alert_high = ALERT_OFF;
        screen_configs[s].quad_bl_alert_low = ALERT_OFF;
        screen_configs[s].quad_bl_alert_high = ALERT_OFF;
        screen_configs[s].quad_br_alert_low = ALERT_OFF;
        screen_configs[s].quad_br_alert_high = ALERT_OFF;
        screen_configs[s].gauge_alert_low = ALERT_OFF;
        screen_configs[s].gauge_alert_high = ALERT_OFF;
    }
    if (preferences.begin(SCREENS_NAMESPACE, true)) {
        for (int s = 0; s < NUM_SCREENS; s++) {
            char key[12];
            snprintf(key, sizeof(key), "scr%d", s);
            size_t len = preferences.getBytesLength(key);
            if (len > 0 && len <= sizeof(RlcdScreenConfig)) {
                // Load even if saved with older/smaller struct — defaults fill the rest
                preferences.getBytes(key, &screen_configs[s], len);
            } else if (len > sizeof(RlcdScreenConfig)) {
                // Saved with newer/larger struct — load what fits
                preferences.getBytes(key, &screen_configs[s], sizeof(RlcdScreenConfig));
            }
        }
        preferences.end();
        Serial.println("[NVS] Screen configs loaded");
    }
}

// ─── Public accessors ───────────────────────────────────────────────────────

bool is_wifi_connected() {
    return WiFi.status() == WL_CONNECTED;
}

String get_signalk_server_ip() {
    return saved_signalk_ip;
}

uint16_t get_signalk_server_port() {
    return saved_signalk_port;
}

// ─── RSSI bar helper ────────────────────────────────────────────────────────

static String rssi_bar(int rssi) {
    int pct = constrain(2 * (rssi + 100), 0, 100);
    String color = (pct > 60) ? "#2a2" : (pct > 30) ? "#da2" : "#d22";
    return String(rssi) + " dBm (" + String(pct) + "%) "
         + "<span style='display:inline-block;width:80px;background:#eee;border-radius:3px;height:12px;vertical-align:middle'>"
         + "<span style='display:inline-block;width:" + String(pct) + "%;background:" + color + ";height:100%;border-radius:3px'></span></span>";
}

// ─── Home page ──────────────────────────────────────────────────────────────

static void handle_root() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>RLCD Marine Display</title></head><body><div class='container'>";
    html += "<div class='tab-content' style='text-align:center;'>";
    html += "<h1>RLCD Marine Display</h1>";
    html += "<div class='status'>Status: " + String(WiFi.isConnected() ? "Connected" : "AP Mode");
    html += "<br>IP: " + (WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
    if (saved_hostname.length()) {
        html += "<br>Hostname: " + saved_hostname + ".local";
    }
    html += "</div>";

    // Screen selector
    html += "<div style='background:#f4f6fa;border:1px solid #d8e0ef;border-radius:6px;padding:10px;margin-top:12px;'>";
    html += "<div style='font-weight:bold;margin-bottom:8px;'>Screens</div>";
    html += "<div style='display:flex;justify-content:center;gap:8px;flex-wrap:wrap;'>";
    int cs = get_current_screen();
    for (int i = 0; i < NUM_SCREENS; i++) {
        String redirect = "/set-screen?screen=" + String(i);
        if (i == cs) {
            html += "<button class='tab-btn' style='background:#dbe8ff;font-weight:700' onclick=\"location.href='" + redirect + "'\">Screen " + String(i+1) + "</button>";
        } else {
            html += "<button class='tab-btn' onclick=\"location.href='" + redirect + "'\">Screen " + String(i+1) + "</button>";
        }
    }
    html += "</div></div>";

    html += "<div class='root-actions' style='margin-top:12px;'>";
    html += "<button class='tab-btn' onclick=\"location.href='/network'\">Network Setup</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/screens'\">Screen Config</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/device'\">Device Settings</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/update'\">Firmware Update</button>";
    html += "</div>";

    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

// ─── WiFi scan ──────────────────────────────────────────────────────────────

static void handle_scan_wifi() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\"");
        json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
              + ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    config_server.send(200, "application/json", json);
}

// ─── Network setup page ─────────────────────────────────────────────────────

static void handle_network_page() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>Network Setup</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Network Setup</h2>";

    // WiFi status panel
    if (WiFi.isConnected()) {
        html += "<div style='background:#e8f5e9;border:1px solid #a5d6a7;border-radius:6px;padding:10px;margin-bottom:14px;text-align:center'>";
        html += "<b>Connected to:</b> " + WiFi.SSID() + "<br>";
        html += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
        html += "<b>Signal:</b> " + rssi_bar(WiFi.RSSI());
        html += "</div>";
    } else {
        html += "<div style='background:#fff3e0;border:1px solid #ffcc80;border-radius:6px;padding:10px;margin-bottom:14px;text-align:center'>";
        html += "<b>WiFi:</b> Not connected (AP Mode)<br>";
        html += "<b>AP IP:</b> " + WiFi.softAPIP().toString();
        html += "</div>";
    }

    html += "<form method='POST' action='/save-wifi'>";
    html += "<div class='form-row'><label>SSID:</label>"
            "<input id='ssid' name='ssid' type='text' value='" + saved_ssid + "' style='width:40%'>"
            " <button type='button' id='scanBtn' onclick='scanWifi()' style='padding:6px 12px;cursor:pointer'>Scan</button>"
            "</div>";
    html += "<div id='scanResults' style='margin:0 0 10px 148px;display:none'></div>";
    html += "<div class='form-row'><label>Password:</label><input name='password' type='password' value='" + saved_password + "'></div>";
    html += "<div class='form-row'><label>SignalK Server:</label><input name='signalk_ip' type='text' value='" + saved_signalk_ip + "'></div>";
    html += "<div class='form-row'><label>SignalK Port:</label><input name='signalk_port' type='number' value='" + String(saved_signalk_port) + "'></div>";
    html += "<div class='form-row'><label>Hostname:</label><input name='hostname' type='text' value='" + saved_hostname + "'></div>";
    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save & Reboot</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";

    // WiFi scan JavaScript
    html += "<script>"
            "function scanWifi(){"
              "var btn=document.getElementById('scanBtn');"
              "var div=document.getElementById('scanResults');"
              "btn.disabled=true;btn.textContent='Scanning...';"
              "div.style.display='block';div.innerHTML='Scanning...';"
              "fetch('/scan-wifi').then(r=>r.json()).then(nets=>{"
                "if(!nets.length){div.innerHTML='No networks found.';btn.disabled=false;btn.textContent='Scan';return;}"
                "nets.sort((a,b)=>b.rssi-a.rssi);"
                "var seen={};"
                "var t='<table style=\"width:100%;border-collapse:collapse;font-size:0.9em\">';"
                "t+='<tr style=\"background:#e3edf7\"><th style=\"padding:4px 6px;text-align:left\">SSID</th><th>Signal</th><th>Sec</th><th></th></tr>';"
                "nets.forEach(n=>{"
                  "if(seen[n.ssid])return;seen[n.ssid]=1;"
                  "var pct=Math.min(100,Math.max(0,2*(n.rssi+100)));"
                  "var col=pct>60?'#2a2':pct>30?'#da2':'#d22';"
                  "var bar='<span style=\"display:inline-block;width:60px;background:#eee;border-radius:3px;height:10px\">"
                    "<span style=\"display:inline-block;width:'+pct+'%;background:'+col+';height:100%;border-radius:3px\"></span></span> '+n.rssi+'dBm';"
                  "t+='<tr style=\"border-bottom:1px solid #ddd\"><td style=\"padding:4px 6px\">'+n.ssid+'</td><td style=\"text-align:center\">'+bar+'</td>"
                    "<td style=\"text-align:center\">'+(n.enc?'&#128274;':'Open')+'</td>"
                    "<td><button type=\"button\" style=\"padding:2px 8px;cursor:pointer\" onclick=\"pickSsid(\\''+n.ssid.replace(/'/g,\"\\\\'\")+'\\')\">&rarr;</button></td></tr>';"
                "});"
                "t+='</table>';div.innerHTML=t;"
                "btn.disabled=false;btn.textContent='Scan';"
              "}).catch(e=>{div.innerHTML='Scan failed: '+e;btn.disabled=false;btn.textContent='Scan';});"
            "}"
            "function pickSsid(s){document.getElementById('ssid').value=s;}"
            "</script>";

    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

// ─── Save WiFi ──────────────────────────────────────────────────────────────

static void handle_save_wifi() {
    if (config_server.method() != HTTP_POST) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    saved_ssid = config_server.arg("ssid");
    saved_password = config_server.arg("password");
    saved_signalk_ip = config_server.arg("signalk_ip");
    saved_signalk_port = config_server.arg("signalk_port").toInt();
    saved_hostname = config_server.arg("hostname");
    save_preferences();

    Serial.println("[WiFi Config] SSID: " + saved_ssid);
    Serial.println("[WiFi Config] SignalK IP: " + saved_signalk_ip);
    Serial.printf("[WiFi Config] SignalK Port: %u\n", saved_signalk_port);

    String html = "<html><head>";
    html += STYLE;
    html += "<title>Saved</title></head><body><div class='container'>";
    html += "<h2>Settings saved.<br>Rebooting...</h2>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
    delay(1000);
    ESP.restart();
}

// ─── Device settings page ───────────────────────────────────────────────────

static void handle_device_page() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>Device Settings</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Device Settings</h2>";
    html += "<form method='POST' action='/save-device'>";

    // SignalK connection info (read-only summary)
    html += "<div style='background:#f1f7ff;border:1px solid #dbe8ff;border-radius:6px;padding:10px;margin-bottom:14px'>";
    html += "<b>SignalK Server:</b> " + saved_signalk_ip + ":" + String(saved_signalk_port) + "<br>";
    html += "<b>WiFi:</b> " + (WiFi.isConnected() ? WiFi.SSID() : String("AP Mode")) + "<br>";
    html += "<b>Firmware:</b> " + String(FW_VERSION);
    html += "</div>";

    // Unit system
    html += "<div class='form-row'><label>Units:</label><select name='unit_system'>";
    html += "<option value='0'" + String(saved_unit_system==0?" selected":"") + ">Metric</option>";
    html += "<option value='1'" + String(saved_unit_system==1?" selected":"") + ">Imperial US</option>";
    html += "<option value='2'" + String(saved_unit_system==2?" selected":"") + ">Imperial UK</option>";
    html += "<option value='3'" + String(saved_unit_system==3?" selected":"") + ">Nautical Metric</option>";
    html += "<option value='4'" + String(saved_unit_system==4?" selected":"") + ">Nautical Imperial US</option>";
    html += "<option value='5'" + String(saved_unit_system==5?" selected":"") + ">Nautical Imperial UK</option>";
    html += "</select></div>";
    html += "<div id='unit-summary' style='margin:10px 0 14px 148px;padding:8px 12px;background:#f4f6fa;border-radius:6px;font-size:13px;color:#555;line-height:1.6;'></div>";

    // Buzzer mode
    html += "<div class='form-row'><label>Alert Buzzer:</label><select name='buzzer_mode'>";
    html += "<option value='0'" + String(buzzer_mode==0?" selected":"") + ">Off</option>";
    html += "<option value='1'" + String(buzzer_mode==1?" selected":"") + ">Global (all screens)</option>";
    html += "<option value='2'" + String(buzzer_mode==2?" selected":"") + ">Per-screen (active only)</option>";
    html += "</select></div>";

    // Alert volume slider
    html += "<div class='form-row'><label>Alert Volume:</label>";
    html += "<input name='alert_vol' type='range' min='0' max='100' value='" + String(alert_volume) + "' style='width:200px' ";
    html += "oninput=\"document.getElementById('vol_val').textContent=this.value+'%'\">";
    html += "<span id='vol_val' style='margin-left:8px'>" + String(alert_volume) + "%</span></div>";

    // Buzzer cooldown
    html += "<div class='form-row'><label>Buzzer Cooldown:</label><select name='buzzer_cooldown'>";
    const uint16_t cdOpts[] = {0, 5, 10, 30, 60};
    const char* cdNames[] = {"Constant", "5s pause", "10s pause", "30s pause", "60s pause"};
    for (int i = 0; i < 5; i++) {
        html += "<option value='" + String(cdOpts[i]) + "'" + String(buzzer_cooldown_sec==cdOpts[i]?" selected":"") + ">" + String(cdNames[i]) + "</option>";
    }
    html += "</select></div>";

    // Auto-scroll
    html += "<div class='form-row'><label>Auto-scroll:</label><select name='auto_scroll'>";
    html += "<option value='0'"  + String(auto_scroll_sec==0 ?" selected":"") + ">Off</option>";
    html += "<option value='5'"  + String(auto_scroll_sec==5 ?" selected":"") + ">5s</option>";
    html += "<option value='10'" + String(auto_scroll_sec==10?" selected":"") + ">10s</option>";
    html += "<option value='30'" + String(auto_scroll_sec==30?" selected":"") + ">30s</option>";
    html += "<option value='60'" + String(auto_scroll_sec==60?" selected":"") + ">60s</option>";
    html += "</select></div>";
    html += "<script>"
           "var us=document.querySelector('select[name=unit_system]'),ud=document.getElementById('unit-summary');"
           "var info=["
           "'Speed: km/h &bull; Temp: &deg;C &bull; Pressure: bar &bull; Depth: m &bull; Volume: L',"
           "'Speed: mph &bull; Temp: &deg;F &bull; Pressure: PSI &bull; Depth: ft &bull; Volume: US gal',"
           "'Speed: mph &bull; Temp: &deg;C &bull; Pressure: PSI &bull; Depth: ft &bull; Volume: UK gal',"
           "'Speed: kn &bull; Temp: &deg;C &bull; Pressure: bar &bull; Depth: m &bull; Volume: L',"
           "'Speed: kn &bull; Temp: &deg;F &bull; Pressure: PSI &bull; Depth: ft &bull; Volume: US gal',"
           "'Speed: kn &bull; Temp: &deg;C &bull; Pressure: PSI &bull; Depth: ft &bull; Volume: UK gal'"
           "];"
           "function uu(){ud.innerHTML=info[us.value]||'';}"
           "us.addEventListener('change',uu);uu();"
           "</script>";

    // Free heap info
    html += "<div style='font-size:0.85em;color:#888;margin:14px 0'>";
    html += "Free heap: " + String(ESP.getFreeHeap() / 1024) + " KB";
    html += " &bull; PSRAM: " + String(ESP.getFreePsram() / 1024) + " KB";
    html += "</div>";

    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save</button></div>";
    html += "</form>";

    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

static void handle_save_device() {
    if (config_server.method() != HTTP_POST) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    saved_unit_system = (uint8_t)config_server.arg("unit_system").toInt();
    unit_system = (UnitSystem)saved_unit_system;
    buzzer_mode = (uint8_t)config_server.arg("buzzer_mode").toInt();
    if (buzzer_mode > 2) buzzer_mode = 0;
    alert_volume = (uint8_t)config_server.arg("alert_vol").toInt();
    buzzer_cooldown_sec = (uint16_t)config_server.arg("buzzer_cooldown").toInt();
    auto_scroll_sec = (uint16_t)config_server.arg("auto_scroll").toInt();
    save_preferences();
    audio_alert_set_volume(alert_volume);
    refresh_signalk_subscriptions();
    config_server.sendHeader("Location", "/device", true);
    config_server.send(302, "text/plain", "");
}

static void handle_reboot() {
    String html = "<html><head>";
    html += STYLE;
    html += "<meta http-equiv='refresh' content='10;url=/'><title>Rebooting</title></head><body><div class='container'>";
    html += "<h2>Rebooting&hellip;</h2><p>Page will reload in 10 seconds.</p>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
    delay(500);
    ESP.restart();
}

// ─── Screen Configuration page (shell — AJAX loads per-screen fragments) ────

static void handle_screens_page() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>Screen Configuration</title></head><body><div class='container'>";
    html += "<h2>Screen Configuration</h2>";
    html += "<form id='screenForm' method='POST' action='/save-screens'>";
    html += "<input type='hidden' name='save_screen' id='save_screen' value='0'>";

    // Tab bar
    html += "<div style='margin-bottom:16px; text-align:center;'>";
    for (int s = 0; s < NUM_SCREENS; ++s) {
        html += "<button type='button' class='tab-btn' id='tabbtn_" + String(s)
              + "' onclick='showTab(" + String(s)
              + ")' style='margin:0 4px;padding:8px 16px;'>Screen " + String(s+1) + "</button>";
    }
    html += "</div>";

    // Placeholder for AJAX content
    html += "<div id='screen-content' style='min-height:200px;'>";
    html += "<p style='text-align:center;color:#888;padding:40px 0;'>Loading...</p></div>";

    // Save button
    html += "<div style='text-align:center;margin-top:16px;'>";
    html += "<button type='button' id='saveBtn' onclick='ajaxSave()' class='tab-btn' style='padding:10px 24px;'>Save</button></div>";
    html += "</form>";
    html += "<p style='text-align:center;margin-top:10px;'><a href='/'>Back</a></p>";

    // JavaScript
    html += "<script>";
    html += "var NUM=" + String(NUM_SCREENS) + ",cur=-1;";
    html += "function showTab(i){if(i===cur)return;cur=i;document.getElementById('save_screen').value=i;"
            "for(var s=0;s<NUM;s++){var b=document.getElementById('tabbtn_'+s);if(b)b.style.background=s===i?'#dbe8ff':'#f4f6fa';}"
            "var c=document.getElementById('screen-content');c.innerHTML='<p style=\"text-align:center;color:#888;padding:40px 0\">Loading...</p>';"            "fetch('/set-screen?screen='+i).catch(()=>{});"            "fetch('/screens/tab?s='+i).then(r=>r.text()).then(h=>{c.innerHTML=h;toggleType(i);}).catch(e=>{c.innerHTML='<p style=\"color:red\">Error: '+e+'</p>';});}";
    html += "function toggleType(s){"
            "var sel=document.getElementById('dt_'+s);if(!sel)return;"
            "var ids=['numberconfig','dualconfig','quadconfig','graphconfig','compassconfig','positionconfig','gaugeconfig'];"
            "var map={'0':['numberconfig'],'1':['dualconfig'],'2':['quadconfig'],'3':['graphconfig'],'4':['compassconfig'],'5':['positionconfig'],'6':['gaugeconfig']};"
            "var show=map[sel.value]||[];"
            "ids.forEach(d=>{var el=document.getElementById(d+'_'+s);if(el)el.style.display=show.indexOf(d)>=0?'block':'none';});}";
    html += "function ajaxSave(){"
            "var btn=document.getElementById('saveBtn');btn.disabled=true;btn.textContent='Saving...';"
            "var fd=new URLSearchParams(new FormData(document.getElementById('screenForm'))).toString();"
            "fetch('/save-screens',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:fd})"
            ".then(r=>r.json()).then(j=>{btn.disabled=false;btn.textContent='Saved!';setTimeout(()=>{btn.textContent='Save';},2000);})"
            ".catch(e=>{btn.disabled=false;btn.textContent='Error - retry';});}";
    html += "document.addEventListener('DOMContentLoaded',()=>showTab(0));";
    html += "setInterval(function(){fetch('/screens/ping').catch(function(){});},5000);";
    html += "</script>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

// ─── Per-screen AJAX fragment ───────────────────────────────────────────────

static void handle_screens_tab() {
    int s = config_server.arg("s").toInt();
    if (s < 0 || s >= NUM_SCREENS) {
        config_server.send(400, "text/plain", "Bad screen index");
        return;
    }
    const RlcdScreenConfig& cfg = screen_configs[s];
    String html;

    html += "<h3>Screen " + String(s+1) + "</h3>";

    // Helper: show blank for ALERT_OFF, otherwise the number
    auto alertVal = [](int16_t v) -> String {
        return (v == ALERT_OFF) ? String("") : String(v);
    };

    // Display type dropdown
    html += "<div class='form-row'><label>Display Type:</label>";
    html += "<select name='dt_" + String(s) + "' id='dt_" + String(s) + "' onchange='toggleType(" + String(s) + ")'>";
    const char* dtNames[] = {"Number","Dual","Quad","Graph","Compass","Position","Gauge"};
    for (int dt = 0; dt < 7; dt++) {
        html += "<option value='" + String(dt) + "'";
        if (cfg.display_type == dt) html += " selected";
        html += ">" + String(dtNames[dt]) + "</option>";
    }
    html += "</select></div>";

    // ── Number ───────────────────────────────────────────────────────
    html += "<div id='numberconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_NUMBER ? "block" : "none") + "'>";
    html += "<h4>Number Display</h4>";
    html += "<div class='form-row'><label>SignalK Path:</label><input name='num_path_" + String(s) + "' type='text' value='" + String(cfg.number_path) + "'></div>";
    html += "<div class='form-row'><label>Label:</label><input name='num_label_" + String(s) + "' type='text' value='" + String(cfg.number_label) + "' style='width:30%'></div>";
    html += "<div class='form-row'><label>Font Size:</label><select name='num_font_" + String(s) + "'>";
    const char* fnNames[] = {"Small (48pt)","Medium (72pt)","Large (96pt)","XL (120pt)","XXL (144pt)"};
    for (int f = 2; f < 5; f++) {
        html += "<option value='" + String(f) + "'";
        if (cfg.number_font_size == f) html += " selected";
        html += ">" + String(fnNames[f]) + "</option>";
    }
    html += "</select></div>";
    html += "<div class='form-row'><label>Alert Low (blank=off):</label><input name='num_alert_low_" + String(s) + "' type='number' value='" + alertVal(cfg.number_alert_low) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Alert High (blank=off):</label><input name='num_alert_high_" + String(s) + "' type='number' value='" + alertVal(cfg.number_alert_high) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Flash on alert:</label><input name='num_alert_flash_" + String(s) + "' type='checkbox' value='1'" + String(cfg.number_alert_flash ? " checked" : "") + "></div>";
    html += "<div class='form-row'><label>Buzzer on alert:</label><input name='num_alert_buzzer_" + String(s) + "' type='checkbox' value='1'" + String(cfg.number_alert_buzzer ? " checked" : "") + "></div>";
    html += "</div>";
    html += "<div id='dualconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_DUAL ? "block" : "none") + "'>";
    html += "<h4>Dual Display</h4>";
    html += "<h5>Top</h5>";
    html += "<div class='form-row'><label>SignalK Path:</label><input name='dual_top_path_" + String(s) + "' type='text' value='" + String(cfg.dual_top_path) + "'></div>";
    html += "<div class='form-row'><label>Label:</label><input name='dual_top_label_" + String(s) + "' type='text' value='" + String(cfg.dual_top_label) + "' style='width:30%'></div>";
    html += "<div class='form-row'><label>Font Size:</label><select name='dual_top_font_" + String(s) + "'>";
    for (int f = 0; f < 3; f++) {
        html += "<option value='" + String(f) + "'";
        if (cfg.dual_top_font_size == f) html += " selected";
        html += ">" + String(fnNames[f]) + "</option>";
    }
    html += "</select></div>";
    html += "<div class='form-row'><label>Alert Low (blank=off):</label><input name='dual_top_alert_low_" + String(s) + "' type='number' value='" + alertVal(cfg.dual_top_alert_low) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Alert High (blank=off):</label><input name='dual_top_alert_high_" + String(s) + "' type='number' value='" + alertVal(cfg.dual_top_alert_high) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Flash on alert:</label><input name='dual_top_alert_flash_" + String(s) + "' type='checkbox' value='1'" + String(cfg.dual_top_alert_flash ? " checked" : "") + "></div>";
    html += "<div class='form-row'><label>Buzzer on alert:</label><input name='dual_top_alert_buzzer_" + String(s) + "' type='checkbox' value='1'" + String(cfg.dual_top_alert_buzzer ? " checked" : "") + "></div>";
    html += "<h5>Bottom</h5>";
    html += "<div class='form-row'><label>SignalK Path:</label><input name='dual_bot_path_" + String(s) + "' type='text' value='" + String(cfg.dual_bottom_path) + "'></div>";
    html += "<div class='form-row'><label>Label:</label><input name='dual_bot_label_" + String(s) + "' type='text' value='" + String(cfg.dual_bottom_label) + "' style='width:30%'></div>";
    html += "<div class='form-row'><label>Font Size:</label><select name='dual_bot_font_" + String(s) + "'>";
    for (int f = 0; f < 3; f++) {
        html += "<option value='" + String(f) + "'";
        if (cfg.dual_bottom_font_size == f) html += " selected";
        html += ">" + String(fnNames[f]) + "</option>";
    }
    html += "</select></div>";
    html += "<div class='form-row'><label>Alert Low (blank=off):</label><input name='dual_bot_alert_low_" + String(s) + "' type='number' value='" + alertVal(cfg.dual_bot_alert_low) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Alert High (blank=off):</label><input name='dual_bot_alert_high_" + String(s) + "' type='number' value='" + alertVal(cfg.dual_bot_alert_high) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Flash on alert:</label><input name='dual_bot_alert_flash_" + String(s) + "' type='checkbox' value='1'" + String(cfg.dual_bot_alert_flash ? " checked" : "") + "></div>";
    html += "<div class='form-row'><label>Buzzer on alert:</label><input name='dual_bot_alert_buzzer_" + String(s) + "' type='checkbox' value='1'" + String(cfg.dual_bot_alert_buzzer ? " checked" : "") + "></div>";
    html += "</div>";  // close dualconfig

    // ── Quad ─────────────────────────────────────────────────────────
    html += "<div id='quadconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_QUAD ? "block" : "none") + "'>";
    html += "<h4>Quad Display</h4>";
    const char* qLabels[] = {"Top-Left","Top-Right","Bottom-Left","Bottom-Right"};
    const char* qPrefixes[] = {"quad_tl","quad_tr","quad_bl","quad_br"};
    const char* qPaths[] = {cfg.quad_tl_path, cfg.quad_tr_path, cfg.quad_bl_path, cfg.quad_br_path};
    const char* qLbls[] = {cfg.quad_tl_label, cfg.quad_tr_label, cfg.quad_bl_label, cfg.quad_br_label};
    uint8_t qFonts[] = {cfg.quad_tl_font_size, cfg.quad_tr_font_size, cfg.quad_bl_font_size, cfg.quad_br_font_size};
    int16_t qAlertLow[] = {cfg.quad_tl_alert_low, cfg.quad_tr_alert_low, cfg.quad_bl_alert_low, cfg.quad_br_alert_low};
    int16_t qAlertHigh[] = {cfg.quad_tl_alert_high, cfg.quad_tr_alert_high, cfg.quad_bl_alert_high, cfg.quad_br_alert_high};
    uint8_t qAlertFlash[] = {cfg.quad_tl_alert_flash, cfg.quad_tr_alert_flash, cfg.quad_bl_alert_flash, cfg.quad_br_alert_flash};
    uint8_t qAlertBuzzer[] = {cfg.quad_tl_alert_buzzer, cfg.quad_tr_alert_buzzer, cfg.quad_bl_alert_buzzer, cfg.quad_br_alert_buzzer};
    for (int q = 0; q < 4; q++) {
        html += "<h5>" + String(qLabels[q]) + "</h5>";
        html += "<div class='form-row'><label>SignalK Path:</label><input name='" + String(qPrefixes[q]) + "_path_" + String(s) + "' type='text' value='" + String(qPaths[q]) + "'></div>";
        html += "<div class='form-row'><label>Label:</label><input name='" + String(qPrefixes[q]) + "_label_" + String(s) + "' type='text' value='" + String(qLbls[q]) + "' style='width:30%'></div>";
        html += "<div class='form-row'><label>Font Size:</label><select name='" + String(qPrefixes[q]) + "_font_" + String(s) + "'>";
        for (int f = 0; f < 2; f++) {
            html += "<option value='" + String(f) + "'";
            if (qFonts[q] == f) html += " selected";
            html += ">" + String(fnNames[f]) + "</option>";
        }
        html += "</select></div>";
        html += "<div class='form-row'><label>Alert Low (blank=off):</label><input name='" + String(qPrefixes[q]) + "_alert_low_" + String(s) + "' type='number' value='" + alertVal(qAlertLow[q]) + "' style='width:20%'></div>";
        html += "<div class='form-row'><label>Alert High (blank=off):</label><input name='" + String(qPrefixes[q]) + "_alert_high_" + String(s) + "' type='number' value='" + alertVal(qAlertHigh[q]) + "' style='width:20%'></div>";
        html += "<div class='form-row'><label>Flash on alert:</label><input name='" + String(qPrefixes[q]) + "_alert_flash_" + String(s) + "' type='checkbox' value='1'" + String(qAlertFlash[q] ? " checked" : "") + "></div>";
        html += "<div class='form-row'><label>Buzzer on alert:</label><input name='" + String(qPrefixes[q]) + "_alert_buzzer_" + String(s) + "' type='checkbox' value='1'" + String(qAlertBuzzer[q] ? " checked" : "") + "></div>";
    }
    html += "</div>";

    // ── Graph ────────────────────────────────────────────────────────
    html += "<div id='graphconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_GRAPH ? "block" : "none") + "'>";
    html += "<h4>Graph Display</h4>";
    html += "<div class='form-row'><label>SignalK Path 1:</label><input name='graph_p1_" + String(s) + "' type='text' value='" + String(cfg.graph_path_1) + "'></div>";
    html += "<div class='form-row'><label>Chart Type:</label><select name='graph_type_" + String(s) + "'>";
    html += "<option value='0'" + String(cfg.graph_chart_type == 0 ? " selected" : "") + ">Line</option>";
    html += "<option value='1'" + String(cfg.graph_chart_type == 1 ? " selected" : "") + ">Bar</option>";
    html += "</select></div>";
    html += "<div class='form-row'><label>Time Range:</label><select name='graph_range_" + String(s) + "'>";
    const char* trNames[] = {"30 seconds","1 minute","5 minutes","10 minutes","30 minutes"};
    for (int t = 0; t < 5; t++) {
        html += "<option value='" + String(t) + "'";
        if (cfg.graph_time_range == t) html += " selected";
        html += ">" + String(trNames[t]) + "</option>";
    }
    html += "</select></div>";
    html += "</div>";

    // ── Compass ──────────────────────────────────────────────────────
    html += "<div id='compassconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_COMPASS ? "block" : "none") + "'>";
    html += "<h4>Compass</h4>";
    bool isMag = (String(cfg.compass_path).length() == 0 || String(cfg.compass_path) == "navigation.headingMagnetic");
    html += "<div class='form-row'><label>Heading:</label>";
    html += "<label style='width:auto'><input type='radio' name='compass_src_" + String(s) + "' value='navigation.headingMagnetic'";
    if (isMag) html += " checked";
    html += "> Magnetic</label> ";
    html += "<label style='width:auto'><input type='radio' name='compass_src_" + String(s) + "' value='navigation.headingTrue'";
    if (!isMag) html += " checked";
    html += "> True</label></div>";
    html += "<h5>Extra Data Fields</h5>";
    html += "<div class='form-row'><label>Bottom-Left:</label><input name='compass_bl_" + String(s) + "' type='text' value='" + String(cfg.compass_bl_path) + "'></div>";
    html += "<div class='form-row'><label>BL Label:</label><input name='compass_bl_label_" + String(s) + "' type='text' value='" + String(cfg.compass_bl_label) + "' style='width:30%'></div>";
    html += "<div class='form-row'><label>Bottom-Right:</label><input name='compass_br_" + String(s) + "' type='text' value='" + String(cfg.compass_br_path) + "'></div>";
    html += "<div class='form-row'><label>BR Label:</label><input name='compass_br_label_" + String(s) + "' type='text' value='" + String(cfg.compass_br_label) + "' style='width:30%'></div>";
    html += "</div>";

    // ── Position ─────────────────────────────────────────────────────
    html += "<div id='positionconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_POSITION ? "block" : "none") + "'>";
    html += "<h4>Position Display</h4>";
    html += "<div class='form-row'><label>Format:</label><select name='pos_fmt_" + String(s) + "'>";
    const char* pfNames[] = {"Decimal Degrees","DMS","Decimal Minutes"};
    for (int p = 0; p < 3; p++) {
        html += "<option value='" + String(p) + "'";
        if (cfg.pos_format == p) html += " selected";
        html += ">" + String(pfNames[p]) + "</option>";
    }
    html += "</select></div>";
    html += "</div>";

    // ── Gauge ──────────────────────────────────────────────────────────────
    html += "<div id='gaugeconfig_" + String(s) + "' style='display:" + String(cfg.display_type == DISPLAY_TYPE_GAUGE ? "block" : "none") + "'>";
    html += "<h4>Gauge</h4>";
    html += "<div class='form-row'><label>SignalK Path:</label><input name='gauge_path_" + String(s) + "' type='text' value='" + String(cfg.gauge_path) + "'></div>";
    html += "<div class='form-row'><label>Label:</label><input name='gauge_label_" + String(s) + "' type='text' value='" + String(cfg.gauge_label) + "' style='width:30%'></div>";
    html += "<div class='form-row'><label>Style:</label><select name='gauge_style_" + String(s) + "'>";
    const char* gsNames[] = {"Radial","Bar"};
    for (int g = 0; g < 2; g++) {
        html += "<option value='" + String(g) + "'";
        if (cfg.gauge_style == g) html += " selected";
        html += ">" + String(gsNames[g]) + "</option>";
    }
    html += "</select></div>";
    html += "<div class='form-row'><label>Min:</label><input name='gauge_min_" + String(s) + "' type='number' value='" + String(cfg.gauge_min) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Max:</label><input name='gauge_max_" + String(s) + "' type='number' value='" + String(cfg.gauge_max) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Alert Low (blank=off):</label><input name='gauge_alert_low_" + String(s) + "' type='number' value='" + alertVal(cfg.gauge_alert_low) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Alert High (blank=off):</label><input name='gauge_alert_high_" + String(s) + "' type='number' value='" + alertVal(cfg.gauge_alert_high) + "' style='width:20%'></div>";
    html += "<div class='form-row'><label>Flash display on alert:</label><input name='gauge_alert_flash_" + String(s) + "' type='checkbox' value='1'" + String(cfg.gauge_alert_flash ? " checked" : "") + "></div>";
    html += "<div class='form-row'><label>Buzzer on alert:</label><input name='gauge_alert_buzzer_" + String(s) + "' type='checkbox' value='1'" + String(cfg.gauge_alert_buzzer ? " checked" : "") + "></div>";
    html += "</div>";

    config_server.send(200, "text/html", html);
}

// ─── Save screen config (AJAX) ──────────────────────────────────────────────

static void handle_save_screens() {
    if (config_server.method() != HTTP_POST) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    int s = config_server.arg("save_screen").toInt();
    if (s < 0 || s >= NUM_SCREENS) {
        config_server.send(400, "application/json", "{\"error\":\"bad screen\"}");
        return;
    }

    auto arg = [&](const String& key) -> String { return config_server.arg(key); };
    auto copyStr = [](const String& src, char* dst, size_t len) {
        memset(dst, 0, len);
        size_t n = src.length();
        if (n >= len) n = len - 1;
        memcpy(dst, src.c_str(), n);
    };
    auto parseAlert = [&](const String& key) -> int16_t {
        String v = config_server.arg(key);
        v.trim();
        return v.length() == 0 ? ALERT_OFF : (int16_t)v.toInt();
    };

    RlcdScreenConfig& cfg = screen_configs[s];
    cfg.display_type = arg("dt_" + String(s)).toInt();

    // Number
    copyStr(arg("num_path_" + String(s)), cfg.number_path, sizeof(cfg.number_path));
    copyStr(arg("num_label_" + String(s)), cfg.number_label, sizeof(cfg.number_label));
    cfg.number_font_size = arg("num_font_" + String(s)).toInt();
    cfg.number_alert_low = parseAlert("num_alert_low_" + String(s));
    cfg.number_alert_high = parseAlert("num_alert_high_" + String(s));
    cfg.number_alert_flash = config_server.hasArg("num_alert_flash_" + String(s)) ? 1 : 0;
    cfg.number_alert_buzzer = config_server.hasArg("num_alert_buzzer_" + String(s)) ? 1 : 0;

    // Dual
    copyStr(arg("dual_top_path_" + String(s)), cfg.dual_top_path, sizeof(cfg.dual_top_path));
    copyStr(arg("dual_top_label_" + String(s)), cfg.dual_top_label, sizeof(cfg.dual_top_label));
    cfg.dual_top_font_size = arg("dual_top_font_" + String(s)).toInt();
    cfg.dual_top_alert_low = parseAlert("dual_top_alert_low_" + String(s));
    cfg.dual_top_alert_high = parseAlert("dual_top_alert_high_" + String(s));
    cfg.dual_top_alert_flash = config_server.hasArg("dual_top_alert_flash_" + String(s)) ? 1 : 0;
    cfg.dual_top_alert_buzzer = config_server.hasArg("dual_top_alert_buzzer_" + String(s)) ? 1 : 0;
    copyStr(arg("dual_bot_path_" + String(s)), cfg.dual_bottom_path, sizeof(cfg.dual_bottom_path));
    copyStr(arg("dual_bot_label_" + String(s)), cfg.dual_bottom_label, sizeof(cfg.dual_bottom_label));
    cfg.dual_bottom_font_size = arg("dual_bot_font_" + String(s)).toInt();
    cfg.dual_bot_alert_low = parseAlert("dual_bot_alert_low_" + String(s));
    cfg.dual_bot_alert_high = parseAlert("dual_bot_alert_high_" + String(s));
    cfg.dual_bot_alert_flash = config_server.hasArg("dual_bot_alert_flash_" + String(s)) ? 1 : 0;
    cfg.dual_bot_alert_buzzer = config_server.hasArg("dual_bot_alert_buzzer_" + String(s)) ? 1 : 0;

    // Quad
    const char* qp[] = {"quad_tl","quad_tr","quad_bl","quad_br"};
    char* qPaths[] = {cfg.quad_tl_path, cfg.quad_tr_path, cfg.quad_bl_path, cfg.quad_br_path};
    char* qLabels[] = {cfg.quad_tl_label, cfg.quad_tr_label, cfg.quad_bl_label, cfg.quad_br_label};
    uint8_t* qFonts[] = {&cfg.quad_tl_font_size, &cfg.quad_tr_font_size, &cfg.quad_bl_font_size, &cfg.quad_br_font_size};
    int16_t* qAlertLow[] = {&cfg.quad_tl_alert_low, &cfg.quad_tr_alert_low, &cfg.quad_bl_alert_low, &cfg.quad_br_alert_low};
    int16_t* qAlertHigh[] = {&cfg.quad_tl_alert_high, &cfg.quad_tr_alert_high, &cfg.quad_bl_alert_high, &cfg.quad_br_alert_high};
    uint8_t* qAlertFlash[] = {&cfg.quad_tl_alert_flash, &cfg.quad_tr_alert_flash, &cfg.quad_bl_alert_flash, &cfg.quad_br_alert_flash};
    uint8_t* qAlertBuzzer[] = {&cfg.quad_tl_alert_buzzer, &cfg.quad_tr_alert_buzzer, &cfg.quad_bl_alert_buzzer, &cfg.quad_br_alert_buzzer};
    size_t pathSz[] = {sizeof(cfg.quad_tl_path), sizeof(cfg.quad_tr_path), sizeof(cfg.quad_bl_path), sizeof(cfg.quad_br_path)};
    size_t lblSz[] = {sizeof(cfg.quad_tl_label), sizeof(cfg.quad_tr_label), sizeof(cfg.quad_bl_label), sizeof(cfg.quad_br_label)};
    for (int q = 0; q < 4; q++) {
        copyStr(arg(String(qp[q]) + "_path_" + String(s)), qPaths[q], pathSz[q]);
        copyStr(arg(String(qp[q]) + "_label_" + String(s)), qLabels[q], lblSz[q]);
        *qFonts[q] = arg(String(qp[q]) + "_font_" + String(s)).toInt();
        *qAlertLow[q] = parseAlert(String(qp[q]) + "_alert_low_" + String(s));
        *qAlertHigh[q] = parseAlert(String(qp[q]) + "_alert_high_" + String(s));
        *qAlertFlash[q] = config_server.hasArg(String(qp[q]) + "_alert_flash_" + String(s)) ? 1 : 0;
        *qAlertBuzzer[q] = config_server.hasArg(String(qp[q]) + "_alert_buzzer_" + String(s)) ? 1 : 0;
    }

    // Graph
    copyStr(arg("graph_p1_" + String(s)), cfg.graph_path_1, sizeof(cfg.graph_path_1));
    copyStr(arg("graph_p2_" + String(s)), cfg.graph_path_2, sizeof(cfg.graph_path_2));
    cfg.graph_chart_type = arg("graph_type_" + String(s)).toInt();
    cfg.graph_time_range = arg("graph_range_" + String(s)).toInt();

    // Compass
    copyStr(arg("compass_src_" + String(s)), cfg.compass_path, sizeof(cfg.compass_path));
    copyStr(arg("compass_bl_" + String(s)), cfg.compass_bl_path, sizeof(cfg.compass_bl_path));
    copyStr(arg("compass_bl_label_" + String(s)), cfg.compass_bl_label, sizeof(cfg.compass_bl_label));
    copyStr(arg("compass_br_" + String(s)), cfg.compass_br_path, sizeof(cfg.compass_br_path));
    copyStr(arg("compass_br_label_" + String(s)), cfg.compass_br_label, sizeof(cfg.compass_br_label));

    // Position
    cfg.pos_format = arg("pos_fmt_" + String(s)).toInt();

    // Gauge
    copyStr(arg("gauge_path_" + String(s)), cfg.gauge_path, sizeof(cfg.gauge_path));
    copyStr(arg("gauge_label_" + String(s)), cfg.gauge_label, sizeof(cfg.gauge_label));
    cfg.gauge_style = arg("gauge_style_" + String(s)).toInt();
    cfg.gauge_min = arg("gauge_min_" + String(s)).toInt();
    cfg.gauge_max = arg("gauge_max_" + String(s)).toInt();
    cfg.gauge_alert_low = parseAlert("gauge_alert_low_" + String(s));
    cfg.gauge_alert_high = parseAlert("gauge_alert_high_" + String(s));
    cfg.gauge_alert_flash = config_server.hasArg("gauge_alert_flash_" + String(s)) ? 1 : 0;
    cfg.gauge_alert_buzzer = config_server.hasArg("gauge_alert_buzzer_" + String(s)) ? 1 : 0;

    save_screen_configs();
    // Switch to the screen that was just configured
    set_current_screen(s);
    // Update SignalK subscriptions to match new config
    refresh_signalk_subscriptions();
    Serial.printf("[SAVE] Screen %d saved (type=%d)\n", s, cfg.display_type);
    config_server.send(200, "application/json", "{\"ok\":true,\"screen\":" + String(s) + "}");
}

// ─── OTA firmware update ────────────────────────────────────────────────────

static void handle_ota_page() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
    char info[128];
    snprintf(info, sizeof(info),
             "Running: %s @ 0x%06lX (%lu KB) — Next: %s @ 0x%06lX",
             running ? running->label : "?",
             running ? (unsigned long)running->address : 0UL,
             running ? (unsigned long)(running->size / 1024) : 0UL,
             next    ? next->label : "none",
             next    ? (unsigned long)next->address : 0UL);

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += STYLE;
    html += "<title>OTA Firmware Update</title></head><body><div class='container'>";
    html += "<h2>Firmware Update</h2>";
    html += "<p style='font-size:0.85em;color:#888'>" + String(info) + "</p>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<p>Select a <code>.bin</code> firmware file:</p>";
    html += "<input type='file' name='firmware' accept='.bin' required style='margin-bottom:12px'><br>";
    html += "<input type='submit' value='Upload &amp; Flash' "
            "onclick=\"this.disabled=true;this.value='Flashing&hellip;';this.form.submit()\">";
    html += "</form>";
    html += "<p style='margin-top:20px'><a href='/'>&#8592; Back</a></p>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

static void handle_ota_upload() {
    HTTPUpload& upload = config_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        esp_task_wdt_reset();
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        esp_task_wdt_reset();
        if (Update.end(true)) {
            Serial.printf("[OTA] Success: %u bytes\n", (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

static void handle_ota_post() {
    bool ok = !Update.hasError();
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    if (ok) html += "<meta http-equiv='refresh' content='20;url=/'>";
    html += STYLE;
    html += "<title>OTA Update</title></head><body><div class='container'>";
    if (ok) {
        html += "<h3>Update successful</h3>";
        html += "<p>Device is rebooting&hellip; page will reload in 20 seconds.</p>";
    } else {
        html += "<h3>Update FAILED</h3>";
        html += "<p>" + String(Update.errorString()) + "</p>";
        html += "<p><a href='/update'>Try again</a></p>";
    }
    html += "</div></body></html>";
    config_server.send(ok ? 200 : 500, "text/html", html);

    if (ok) {
        config_server.client().flush();
        delay(200);
        Serial.println("[OTA] Restarting...");
        Serial.flush();
        esp_restart();
    }
}

// ─── setup_network — main entry point ───────────────────────────────────────

void setup_network() {
    // Load preferences from NVS
    load_preferences();

    // WiFi: try STA, fall back to AP
    WiFi.mode(WIFI_STA);
    if (saved_hostname.length() > 0) {
        WiFi.setHostname(saved_hostname.c_str());
    }

    if (saved_ssid.length() > 0) {
        WiFi.begin(saved_ssid.c_str(), saved_password.c_str());
        Serial.print("[WiFi] Connecting");
        int tries = 0;
        while (WiFi.status() != WL_CONNECTED && tries < 30) {
            delay(500);
            Serial.print(".");
            tries++;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        if (saved_hostname.length() > 0) {
            if (MDNS.begin(saved_hostname.c_str())) {
                Serial.printf("[mDNS] %s.local\n", saved_hostname.c_str());
            }
        }
    } else {
        Serial.println("\n[WiFi] Failed, starting AP mode");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("RLCD-Marine", "boatdisplay");
        WiFi.setSleep(false);
        Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }

    // Register web UI routes
    config_server.on("/", handle_root);
    config_server.on("/network", handle_network_page);
    config_server.on("/save-wifi", HTTP_POST, handle_save_wifi);
    config_server.on("/scan-wifi", HTTP_GET, handle_scan_wifi);
    config_server.on("/device", handle_device_page);
    config_server.on("/save-device", HTTP_POST, handle_save_device);
    config_server.on("/reboot", HTTP_POST, handle_reboot);
    config_server.on("/screens", []() {
        last_config_activity = millis();
        handle_screens_page();
    });
    config_server.on("/screens/ping", []() {
        last_config_activity = millis();
        config_server.send(204);
    });
    config_server.on("/screens/tab", HTTP_GET, []() {
        last_config_activity = millis();
        handle_screens_tab();
    });
    config_server.on("/save-screens", HTTP_POST, []() {
        last_config_activity = millis();
        handle_save_screens();
    });
    config_server.on("/set-screen", HTTP_GET, []() {
        int s = config_server.arg("screen").toInt();
        if (s < 0 || s >= NUM_SCREENS) s = 0;
        set_current_screen(s);
        subscribe_to_active_screen(s);
        config_server.sendHeader("Location", "/", true);
        config_server.send(302, "text/plain", "");
    });
    config_server.on("/update", HTTP_GET, handle_ota_page);
    config_server.on("/update", HTTP_POST, handle_ota_post, handle_ota_upload);

    config_server.begin();
    Serial.println("[WebServer] Started on port 80");
}
