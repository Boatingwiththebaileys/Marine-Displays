// ...existing code...

#include "gauge_config.h"
#include "version.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <vector>
#include <set>
#include "network_setup.h"
#include "signalk_config.h"
#include "gauge_config.h"
#include "screen_config_c_api.h"
#include "nmea2000_config.h"
#include "ui_Settings.h"
#include <FS.h>
#include <SPIFFS.h>
#include <SD_MMC.h>
#include <dirent.h>
#include <sys/stat.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "attitude_display.h"

// ...existing code...

// Place fallback/error screen logic after all includes and config loads
extern "C" void show_fallback_error_screen_if_needed() {
    // A screen is considered configured if ANY of the following are non-default:
    //   - display_type != GAUGE (NUMBER/DUAL/QUAD/GRAPH/COMPASS/POSITION have no cal points)
    //   - background_path set
    //   - icon_paths set
    //   - number_path / dual_top_path / quad paths set  (NUMBER/DUAL/QUAD screens)
    //   - calibration angles set                        (GAUGE screens with custom cal)
    // Checking ONLY cal angles gives false positives for screens that use default
    // linear mapping or non-gauge display types.
    bool all_default = true;
    for (int s = 0; s < NUM_SCREENS && all_default; ++s) {
        if (screen_configs[s].display_type != 0) { all_default = false; break; }
        if (screen_configs[s].background_path[0] != '\0') { all_default = false; break; }
        if (screen_configs[s].icon_paths[0][0] != '\0') { all_default = false; break; }
        if (screen_configs[s].icon_paths[1][0] != '\0') { all_default = false; break; }
        if (screen_configs[s].number_path[0] != '\0') { all_default = false; break; }
        if (screen_configs[s].dual_top_path[0] != '\0') { all_default = false; break; }
        for (int g = 0; g < 2 && all_default; ++g) {
            for (int p = 0; p < 5; ++p) {
                if (screen_configs[s].cal[g][p].angle != 0 || screen_configs[s].cal[g][p].value != 0.0f) {
                    all_default = false; break;
                }
            }
        }
    }
    if (all_default) {
        Serial.println("[WARN] No valid config loaded. Keeping default UI (logo/settings) so setup remains accessible.");
    }
}

// ...existing code...

#include "gauge_config.h"


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "network_setup.h"
#include "signalk_config.h"
#include "unit_convert.h"
#include "gauge_config.h"
#include "screen_config_c_api.h"
#include "LVGL_Driver.h"
#include "esp_task_wdt.h"
#include <lwip/sockets.h>   // SO_LINGER for RST-on-close (no TIME_WAIT)
#include <FS.h>
#include <SPIFFS.h>
#include <SD_MMC.h>

#include "nvs.h"
#include "nvs_flash.h"
#include <esp_err.h>
#include "esp_log.h"
#include "needle_style.h"
#include "TCA9554PWR.h"

static const char *TAG_SETUP = "network_setup";

// Close the current HTTP client connection with RST so lwIP frees the PCB
// immediately (no TIME_WAIT). Called inside handlers after the response is
// fully sent. Each save cycle opens 3 connections; without RST each PCB
// sits in TIME_WAIT for ~60 s, leaking ~150 bytes iRAM per PCB.
static inline void rst_close_client() {
    WiFiClient cl = config_server.client();
    if (cl) {
        struct linger lg = { 1, 0 };   // l_onoff=1, l_linger=0 -> RST
        setsockopt(cl.fd(), SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        cl.stop();
    }
}

// Expose a small helper to dump loaded screen configs for debugging
void dump_screen_configs(void) {
    ESP_LOGI(TAG_SETUP, "Dumping %u screen_configs", (unsigned)(sizeof(screen_configs)/sizeof(screen_configs[0])));
    size_t total_screens = sizeof(screen_configs)/sizeof(screen_configs[0]);
    for (size_t s = 0; s < total_screens; ++s) {
        ESP_LOGI(TAG_SETUP, "Screen %u: background='%s' icon_top='%s' icon_bottom='%s' show_bottom=%u", (unsigned)s,
                 screen_configs[s].background_path, screen_configs[s].icon_paths[0], screen_configs[s].icon_paths[1], (unsigned)screen_configs[s].show_bottom);
        for (int g = 0; g < 2; ++g) {
            ESP_LOGI(TAG_SETUP, "  Gauge %d:", g);
            for (int p = 0; p < 5; ++p) {
                ESP_LOGI(TAG_SETUP, "    Point %d: angle=%d value=%.3f", p+1, screen_configs[s].cal[g][p].angle, screen_configs[s].cal[g][p].value);
            }
        }
    }
}

// Returns a safe 7-char hex color string ('#RRGGBB') for use in HTML attributes.
// If the char array doesn't contain a valid hex color, returns the fallback.
static String safeColor(const char* c, const char* fallback = "#000000") {
    if (!c || c[0] != '#') return fallback;
    for (int i = 1; i <= 6; i++) {
        if (!c[i]) return fallback;
        char ch = c[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')))
            return fallback;
    }
    return String(c).substring(0, 7);
}

// Returns a sanitized copy of a char-array field with only printable ASCII chars.
// Prevents non-UTF-8 binary bytes from being injected into HTML responses.
static String safeStr(const char* s, size_t maxlen = 128) {
    if (!s) return "";
    size_t len = 0;
    while (len < maxlen && s[len]) len++;
    String result;
    result.reserve(len);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c <= 0x7E) result += (char)c;
    }
    return result;
}

// Minimal HTML style used by the configuration web pages.
// Stored as a plain const char[] so the data stays in flash (.rodata)
// instead of being heap-copied like a const String would be (~2.8 KB saved).
static const char STYLE[] PROGMEM = R"rawliteral(
<style>
body{font-family:Arial,Helvetica,sans-serif;background:#fff;color:#111}
.container{max-width:900px;margin:0 auto;padding:12px}
.tab-btn{background:#f4f6fa;border:1px solid #d8e0ef;border-radius:4px;padding:8px 12px;cursor:pointer}
.tab-content{border:1px solid #e6e9f2;padding:12px;border-radius:6px;background:#fff}
input[type=number]{width:90px}

/* Icon section styling */
.icon-section{display:flex;flex-direction:column;background:linear-gradient(180deg, #f7fbff, #ffffff);border:1px solid #dbe8ff;padding:10px;border-radius:6px;margin-bottom:8px;box-shadow:0 1px 0 rgba(0,0,0,0.02)}
.icon-section > .icon-row{display:flex;gap:12px;align-items:center}
.icon-section label{font-weight:600}
.icon-preview{width:48px;height:48px;border-radius:6px;background:#fff;border:1px solid #e6eefc;display:inline-block;overflow:hidden;display:flex;align-items:center;justify-content:center}
.icon-section .zone-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:6px}
.icon-section .zone-item{min-width:150px}
.icon-section .zone-item.small{min-width:90px}
.icon-section .color-input{width:40px;height:28px;padding:0;border:0;background:transparent}
.tab-content h3{margin-top:0;color:#1f4f8b}
/* Root page helpers */
.status{background:#f1f7ff;border:1px solid #dbe8ff;padding:10px;border-radius:6px;margin-bottom:12px;color:#0b2f5a}
.root-actions{display:flex;justify-content:center;gap:12px;margin-top:8px}
/* Screens selector container */
.screens-container{background:linear-gradient(180deg,#f0f7ff,#ffffff);border:1px solid #cfe6ff;padding:10px;border-radius:8px;margin-bottom:12px;display:flex;flex-direction:column;align-items:center}
.screens-container .screens-row{display:flex;gap:8px;flex-wrap:wrap;justify-content:center}
.screens-container .screens-title{width:100%;text-align:center;margin-bottom:6px;font-weight:700;color:#0b3b6a}
/* Form helpers */
.form-row{display:flex;flex-direction:row;align-items:center;gap:8px;margin-bottom:10px}
.form-row label{width:140px;text-align:right;color:#0b3b6a}
input[type=text],input[type=password]{width:60%;padding:6px;border:1px solid #dfe9fb;border-radius:4px}
input[type=number]{width:120px;padding:6px;border:1px solid #dfe9fb;border-radius:4px}

/* Assets manager styles */
.assets-uploader{display:flex;gap:8px;align-items:center;justify-content:center;margin-bottom:12px}
.assets-uploader input[type=file]{border:1px dashed #cfe3ff;padding:6px;border-radius:4px;background:#fbfdff}
.file-table{width:100%;border-collapse:collapse;margin-top:8px}
.file-table th{background:#f4f8ff;border-bottom:1px solid #dbe8ff;padding:8px;text-align:left;color:#0b3b6a}
.file-table td{padding:8px;border-bottom:1px solid #eef6ff}
.file-actions form{display:inline;margin-right:8px}
.file-size{color:#5877a8}

/* Calibration table styles */
.table{width:auto;border-collapse:collapse;margin-bottom:8px}
.table th{background:#f4f8ff;border-bottom:1px solid #dbe8ff;padding:4px 6px;text-align:left;color:#0b3b6a;font-weight:600;font-size:0.9em}
.table td{padding:4px 6px;border-bottom:1px solid #eef6ff;white-space:nowrap}
.table td:first-child{width:35px;text-align:center}
.table td:last-child{width:65px;text-align:center}
.table input[type=number]{width:65px;padding:3px 4px;font-size:0.9em}

</style>
)rawliteral";

// Forward declaration for toggle test mode handler
void handle_toggle_test_mode();
void handle_test_gauge();
void handle_nvs_test();
void handle_set_screen();
// Device settings handlers
void handle_device_page();
void handle_save_device();
// Needle style handlers (WebUI only)
void handle_needles_page();
void handle_save_needles();
// Asset manager handlers
void handle_assets_page();
void handle_assets_upload();
void handle_assets_upload_post();
void handle_assets_delete();
// OTA firmware update handlers
void handle_ota_page();
void handle_ota_upload();
void handle_ota_post();
// Hot-update helper (apply backgrounds/icons at runtime)
extern bool apply_all_screen_visuals();

WebServer config_server(80);
Preferences preferences;

String saved_ssid = "";
String saved_password = "";
String saved_signalk_ip = "";
uint16_t saved_signalk_port = 0;
// Hostname for the device (editable via Network Setup)
String saved_hostname = "";
// Own ship MMSI for AIS self-filtering (0 = disabled)
String saved_own_mmsi = "";
// 10 SignalK paths: [screen][gauge] => idx = s*2+g
String signalk_paths[NUM_SCREENS * 2];

// Custom display labels — up to 4 per screen, stored in /config/screen_labels.txt.
// Slot mapping per screen (index = screen_idx * 4 + slot):
//   slot 0 — primary:   Number / Dual-top / Quad-TL / GaugeNum-center / Graph-primary
//   slot 1 — secondary: Dual-bottom / Quad-TR / Graph-secondary
//   slot 2 — tertiary:  Quad-BL
//   slot 3 — quaternary:Quad-BR
String g_custom_labels[NUM_SCREENS * 4];

// Returns the custom label for a given screen/slot, or "" if none set.
const char* get_custom_label(int screen_idx, int slot) {
    int i = screen_idx * 4 + slot;
    if (i < 0 || i >= NUM_SCREENS * 4) return "";
    return g_custom_labels[i].c_str();
}
// Auto-scroll interval in seconds (0 = off)
uint16_t auto_scroll_sec = 0;
// Screen-off timeout in minutes (0 = always on)
uint16_t screen_off_timeout_min = 0;
// Skip a single load of preferences when we've just saved, so the UI
// reflects the in-memory `screen_configs` we just updated instead of
// reloading possibly-stale NVS values.
static volatile bool skip_next_load_preferences = false;
// Deferred LVGL rebuild flag: set by HTTP handlers, consumed by loop().
// Never call apply_all_screen_visuals() directly from an HTTP handler —
// doing so modifies LVGL objects from handleClient() context, which races
// with the display DMA flush and corrupts heap after repeated page-builds.
volatile bool g_pending_visual_apply = false;
volatile bool g_error_screen_active = false;
volatile bool g_screens_need_apply[5] = {false, false, false, false, false};
// millis() timestamp of the last config page visit; reset to 0 after WS auto-resume.
unsigned long g_config_page_last_seen = 0;

// Asset file lists — populated once at startup by scan_sd_assets().
// Reusing these in handle_gauges_page() avoids a live SD scan during HTTP
// handling, which causes SD/WiFi DMA contention on ESP32-S3 and drops the
// SK WebSocket connection.
static std::vector<String> g_iconFiles;
static std::vector<String> g_bgFiles;

// Single shared HTML buffer for handle_gauges_page() and handle_gauges_screen().
// Reserved at 8192 to exceed CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096),
// forcing the backing store into PSRAM and freeing ~8 KB of internal RAM.
String g_http_html_buf;
bool   g_http_html_buf_reserved = false;

// ── N2K field dropdown helper ────────────────────────────────────────
// All N2kField values the user can choose from (order = UI order).
static const N2kField n2k_all_fields[] = {
    // Engine #1
    N2K_ENGINE_RPM, N2K_ENGINE_BOOST, N2K_ENGINE_TILT_TRIM,
    N2K_OIL_PRESSURE, N2K_OIL_TEMPERATURE, N2K_COOLANT_TEMPERATURE,
    N2K_ALT_VOLTAGE, N2K_FUEL_RATE, N2K_ENGINE_HOURS,
    N2K_ENGINE_LOAD, N2K_ENGINE_TORQUE, N2K_EXHAUST_TEMPERATURE,
    // Engine #2
    N2K_ENGINE2_RPM, N2K_ENGINE2_OIL_PRESSURE, N2K_ENGINE2_OIL_TEMP,
    N2K_ENGINE2_COOLANT_TEMP, N2K_ENGINE2_ALT_VOLTAGE,
    N2K_ENGINE2_FUEL_RATE, N2K_ENGINE2_HOURS, N2K_ENGINE2_EXHAUST_TEMP,
    // Transmission
    N2K_TRANS_GEAR, N2K_TRANS_OIL_PRESSURE, N2K_TRANS_OIL_TEMP,
    // Trip
    N2K_TRIP_FUEL_USED, N2K_FUEL_RATE_AVG, N2K_FUEL_RATE_ECONOMY,
    // Fluid levels
    N2K_FUEL_LEVEL, N2K_FRESHWATER_LEVEL, N2K_WASTEWATER_LEVEL,
    N2K_OIL_LEVEL, N2K_BLACKWATER_LEVEL, N2K_LIVEWELL_LEVEL,
    N2K_FUEL_CAPACITY, N2K_FRESHWATER_CAPACITY,
    // Navigation
    N2K_HEADING, N2K_MAGNETIC_DEVIATION, N2K_MAGNETIC_VARIATION_HDG,
    N2K_RATE_OF_TURN, N2K_MAGNETIC_VARIATION,
    N2K_LATITUDE, N2K_LONGITUDE, N2K_COG, N2K_SOG,
    N2K_GNSS_ALTITUDE, N2K_GNSS_SATELLITES, N2K_GNSS_HDOP,
    N2K_XTE, N2K_WPT_DISTANCE, N2K_WPT_BEARING, N2K_WPT_VMG, N2K_ETA_TIME,
    N2K_SPEED_WATER, N2K_SPEED_GROUND,
    N2K_LOG_TOTAL, N2K_LOG_TRIP, N2K_LEEWAY,
    // Depth
    N2K_WATER_DEPTH, N2K_DEPTH_BELOW_KEEL, N2K_DEPTH_OFFSET,
    // Wind
    N2K_WIND_SPEED_APPARENT, N2K_WIND_ANGLE_APPARENT,
    N2K_WIND_SPEED_TRUE, N2K_WIND_ANGLE_TRUE,
    // Environment
    N2K_TEMPERATURE, N2K_SEA_TEMPERATURE, N2K_TEMPERATURE_SET,
    N2K_OUTSIDE_TEMP, N2K_OUTSIDE_HUMIDITY,
    N2K_ATMOSPHERIC_PRESSURE, N2K_BAROMETRIC_PRESSURE,
    // PGN 130316 — Temperature Extended Range
    N2K_FRIDGE_TEMPERATURE, N2K_FREEZER_TEMPERATURE,
    N2K_INSIDE_TEMPERATURE, N2K_CABIN_TEMPERATURE,
    N2K_HEATING_TEMPERATURE, N2K_DEWPOINT_TEMPERATURE,
    N2K_WINDCHILL_APPARENT, N2K_WINDCHILL_THEORETICAL,
    // Attitude
    N2K_PITCH, N2K_ROLL, N2K_YAW,
    // Rudder / Trim
    N2K_RUDDER_POSITION, N2K_TRIM_TAB_PORT, N2K_TRIM_TAB_STBD,
    // Battery / Electrical
    N2K_BATTERY_VOLTAGE, N2K_BATTERY_CURRENT, N2K_BATTERY_TEMPERATURE,
    N2K_BATTERY2_VOLTAGE, N2K_BATTERY2_CURRENT,
    N2K_BATTERY_SOC, N2K_BATTERY_TIME_REMAIN,
    N2K_DC_SOURCE_VOLTAGE, N2K_DC_SOURCE_CURRENT,
    // Time
    N2K_GNSS_DATETIME, N2K_SYSTEM_TIME,
};
static const int n2k_all_fields_count = sizeof(n2k_all_fields) / sizeof(n2k_all_fields[0]);

// Build an HTML <select> for an N2K field, appending directly to `out`
// (which should be the PSRAM-backed html buffer).  This avoids allocating
// a large local String in iRAM — the old return-by-value version peaked at
// ~4096 bytes of iRAM per call and caused OOM hangs on the 2nd config page
// visit when iRAM was already low from TCP send-buffer pbufs.
// Generates a hidden input (submitted), a dropdown of known N2K fields, and a
// text input revealed when "Custom path..." is chosen.  The save handler reads
// only the hidden input (formName) so it needs no changes.  If the stored value
// is not a recognised N2K enum (e.g. a raw SK path string), it pre-selects
// "Custom path..." and pre-fills the text input.
static void n2k_select(String& out, const String& formName, const char* currentVal) {
    bool hasValue = currentVal && currentVal[0] != '\0';
    N2kField cur = n2k_field_from_stored(String(currentVal));
    bool isCustom = hasValue && (cur == N2K_NONE);  // not a recognised N2K enum

    // IDs for the three elements (formName is safe for HTML ids: only [A-Za-z0-9_])
    String hidId = formName + "_v";
    String selId = formName + "_s";
    String txtId = formName + "_t";

    // Stored value for hidden input and data-val attribute:
    //   - known N2K field → numeric enum string (e.g. "50")
    //   - custom path     → the raw path string
    //   - nothing         → "0"
    const char* storedVal = hasValue ? currentVal : "0";

    // Hidden input — the only element with name=formName, always submitted
    out += "<input type='hidden' name='"; out += formName;
    out += "' id='"; out += hidId;
    out += "' value='"; out += storedVal; out += "'>";

    // Dropdown — no name, no options (populated by populateN2kSelects() in JS shell).
    // data-val carries the currently stored value so JS can set the right selection.
    out += "<select id='"; out += selId;
    out += "' class='n2k-sel' data-val='"; out += storedVal; out += "'";
    out += " style='width:70%' onchange=\"(function(v){"
           "var t=document.getElementById('"; out += txtId; out += "');"
           "var h=document.getElementById('"; out += hidId; out += "');"
           "if(v==='__custom__'){t.style.display='inline-block';t.focus();}"
           "else{t.style.display='none';h.value=v;}"
           "})(this.value)\">";
    out += "</select>";

    // Text input — shown only in custom mode, updates the hidden input on every keystroke
    out += "<input type='text' id='"; out += txtId;
    out += "' placeholder='e.g. environment.wind.speedApparent'";
    out += " style='width:60%;margin-top:4px;display:";
    out += (isCustom ? "inline-block" : "none"); out += ";'";
    if (isCustom) { out += " value='"; out += currentVal; out += "'"; }
    out += " oninput=\"document.getElementById('"; out += hidId; out += "').value=this.value;\">";
}

// Overload for String-based current values (gauge signalk_paths[])
static void n2k_select(String& out, const String& formName, const String& currentVal) {
    n2k_select(out, formName, currentVal.c_str());
}

static void scan_sd_assets() {
    g_iconFiles.clear();
    g_bgFiles.clear();
    File root = SD_MMC.open("/assets");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            String fname = file.name();
            file = root.openNextFile(); // advance before processing
            if (fname.startsWith("._") || fname.startsWith("_")) continue;
            String lname = fname;
            lname.toLowerCase();
            String fullPath = fname.startsWith("/assets/") ? fname : "/assets/" + fname;
            if (lname.endsWith(".png"))      g_iconFiles.push_back(String("S:/") + fullPath);
            else if (lname.endsWith(".bin")) g_bgFiles.push_back(String("S:/") + fullPath);
        }
    } else {
        // POSIX fallback
        DIR *d = opendir("/sdcard/assets");
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                const char *fname = entry->d_name;
                if (!fname || fname[0] == '.') continue;
                String sname = String(fname);
                String lname = sname; lname.toLowerCase();
                if (lname.startsWith("_")) continue;
                String fullPath = "/assets/" + sname;
                if (lname.endsWith(".png"))      g_iconFiles.push_back(String("S:/") + fullPath);
                else if (lname.endsWith(".bin")) g_bgFiles.push_back(String("S:/") + fullPath);
            }
            closedir(d);
        }
    }
    Serial.printf("[ASSET SCAN] Cached %u bg files, %u icon files\n",
                  (unsigned)g_bgFiles.size(), (unsigned)g_iconFiles.size());
}
// Namespaces used for Preferences / NVS
const char* SETTINGS_NAMESPACE = "settings";
const char* PREF_NAMESPACE = "gaugeconfig";

// Ensure ScreenConfig/screen_configs symbol visible
#include "screen_config_c_api.h"

// Expose runtime settings from other modules
extern int buzzer_mode;
extern uint16_t buzzer_cooldown_sec;
extern bool first_run_buzzer;
extern unsigned long last_buzzer_time;
extern uint8_t LCD_Backlight;
// UI control helpers (implemented in ui.c)
extern "C" int ui_get_current_screen(void);
extern "C" void ui_set_screen(int screen_num);

void save_preferences(bool skip_screen_blobs = false) {
    preferences.end();
    if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
        Serial.println("[ERROR] preferences.begin failed for settings namespace");
    } else {
        preferences.putString("ssid", saved_ssid);
        preferences.putString("password", saved_password);
        preferences.putString("signalk_ip", saved_signalk_ip);
        preferences.putString("hostname", saved_hostname);
        preferences.putString("own_mmsi", saved_own_mmsi);
        preferences.putUShort("signalk_port", saved_signalk_port);
        // Persist device settings
        preferences.putUShort("buzzer_mode", (uint16_t)buzzer_mode);
        preferences.putUShort("buzzer_cooldown", buzzer_cooldown_sec);
        preferences.putUShort("brightness", (uint16_t)LCD_Backlight);
        // Save auto-scroll setting
        preferences.putUShort("auto_scroll", auto_scroll_sec);
        preferences.putUShort("unit_system", (uint16_t)unit_system);
        preferences.putUShort("screen_off_tmout", screen_off_timeout_min);
        for (int i = 0; i < NUM_SCREENS * 2; ++i) {
            String key = String("skpath_") + i;
            preferences.putString(key.c_str(), signalk_paths[i]);
        }
        preferences.end();
    }

    // Try to save per-screen blobs via NVS (skipped when SD writes succeeded to avoid iRAM NVS page-cache growth)
    nvs_handle_t nvs_handle;
    esp_err_t nvs_err = skip_screen_blobs ? ESP_ERR_NVS_NOT_FOUND : nvs_open(PREF_NAMESPACE, NVS_READWRITE, &nvs_handle);
    bool any_nvs_ok = skip_screen_blobs;
    bool nvs_invalid_length_detected = false;
    const size_t CHUNK_SIZE = 128;
    if (!skip_screen_blobs && nvs_err == ESP_OK) {
        for (int s = 0; s < NUM_SCREENS; ++s) {
            // copy runtime calibration into screen_configs
            for (int g = 0; g < 2; ++g) for (int p = 0; p < 5; ++p) screen_configs[s].cal[g][p] = gauge_cal[s][g][p];
            char key[32];
            snprintf(key, sizeof(key), "screen%d", s);
            esp_err_t err = nvs_set_blob(nvs_handle, key, &screen_configs[s], sizeof(ScreenConfig));
            if (err != ESP_OK) {
                esp_err_t erase_err = nvs_erase_key(nvs_handle, key);
                Serial.printf("[NVS SAVE] nvs_erase_key('%s') -> %d\n", key, erase_err);
                if (erase_err == ESP_OK) {
                    err = nvs_set_blob(nvs_handle, key, &screen_configs[s], sizeof(ScreenConfig));
                    Serial.printf("[NVS SAVE] Retry nvs_set_blob('%s') -> %d\n", key, err);
                }
            }
            if (err == ESP_OK) {
                any_nvs_ok = true;
                continue;
            }
            if (err == ESP_ERR_NVS_INVALID_LENGTH) {
                nvs_invalid_length_detected = true;
            }
            // chunked fallback
            size_t total = sizeof(ScreenConfig);
            int parts = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;
            bool parts_ok = true;
            for (int part = 0; part < parts; ++part) {
                snprintf(key, sizeof(key), "screen%d.part%d", s, part);
                size_t part_sz = ((part + 1) * CHUNK_SIZE > total) ? (total - part * CHUNK_SIZE) : CHUNK_SIZE;
                esp_err_t perr = nvs_set_blob(nvs_handle, key, ((uint8_t *)&screen_configs[s]) + part * CHUNK_SIZE, part_sz);
                if (perr != ESP_OK) { parts_ok = false; break; }
            }
            if (parts_ok) {
                any_nvs_ok = true;
            }
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    } else if (!skip_screen_blobs) {
        Serial.printf("[ERROR] nvs_open failed: %d\n", nvs_err);
    }

    // If we detected systematic NVS invalid-length errors, attempt a repair
    if (nvs_invalid_length_detected) {
        const char *repair_marker = "/config/.nvs_repaired";
        if (!SD_MMC.exists(repair_marker)) {
            Serial.println("[NVS REPAIR] Detected invalid-length errors; attempting NVS repair (erase+init)");
            // Backup settings to SD
            if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
            File bst = SD_MMC.open("/config/nvs_backup_settings.txt", FILE_WRITE);
            if (bst) {
                bst.println(saved_ssid);
                bst.println(saved_password);
                bst.println(saved_signalk_ip);
                bst.println(String(saved_signalk_port));
                for (int i = 0; i < NUM_SCREENS * 2; ++i) bst.println(signalk_paths[i]);
                bst.flush();
                bst.close();
                Serial.println("[NVS REPAIR] Wrote /config/nvs_backup_settings.txt");
            } else {
                Serial.println("[NVS REPAIR] Failed to write settings backup to SD");
            }
            // Backup screen configs
            File bsf = SD_MMC.open("/config/nvs_backup_screens.bin", FILE_WRITE);
            if (bsf) {
                bsf.write((const uint8_t *)screen_configs, sizeof(ScreenConfig) * NUM_SCREENS);
                bsf.flush();
                bsf.close();
                Serial.println("[NVS REPAIR] Wrote /config/nvs_backup_screens.bin");
            } else {
                Serial.println("[NVS REPAIR] Failed to write screens backup to SD");
            }

            // Erase and re-init NVS
            esp_err_t erase_res = nvs_flash_erase();
            Serial.printf("[NVS REPAIR] nvs_flash_erase() -> %d\n", erase_res);
            esp_err_t init_res = nvs_flash_init();
            Serial.printf("[NVS REPAIR] nvs_flash_init() -> %d\n", init_res);

            // Retry writing Preferences and NVS blobs once
            if (init_res == ESP_OK) {
                // Restore preferences (SSID/password etc)
                if (preferences.begin(SETTINGS_NAMESPACE, false)) {
                    preferences.putString("ssid", saved_ssid);
                    preferences.putString("password", saved_password);
                    preferences.putString("signalk_ip", saved_signalk_ip);
                    preferences.putString("hostname", saved_hostname);
                    preferences.putUShort("signalk_port", saved_signalk_port);
                    preferences.putUShort("buzzer_mode", (uint16_t)buzzer_mode);
                    preferences.putUShort("buzzer_cooldown", buzzer_cooldown_sec);
                    preferences.putUShort("auto_scroll", auto_scroll_sec);
                    preferences.putUShort("unit_system", (uint16_t)unit_system);
                    preferences.putUShort("screen_off_tmout", screen_off_timeout_min);
                    for (int i = 0; i < NUM_SCREENS * 2; ++i) {
                        String key = String("skpath_") + i;
                        preferences.putString(key.c_str(), signalk_paths[i]);
                    }
                    preferences.end();
                }

                nvs_handle_t nh2;
                if (nvs_open(PREF_NAMESPACE, NVS_READWRITE, &nh2) == ESP_OK) {
                    bool any_ok2 = false;
                    for (int s = 0; s < NUM_SCREENS; ++s) {
                        char key[32];
                        snprintf(key, sizeof(key), "screen%d", s);
                        esp_err_t r2 = nvs_set_blob(nh2, key, &screen_configs[s], sizeof(ScreenConfig));
                        Serial.printf("[NVS REPAIR] Retry nvs_set_blob('%s') -> %d\n", key, r2);
                        if (r2 == ESP_OK) any_ok2 = true;
                    }
                    nvs_commit(nh2);
                    nvs_close(nh2);
                    // create marker file so we don't repeat erase
                    File mf = SD_MMC.open(repair_marker, FILE_WRITE);
                    if (mf) { mf.print("1"); mf.close(); Serial.println("[NVS REPAIR] Marker written"); }
                    if (any_ok2) {
                        Serial.println("[NVS REPAIR] Repair appeared successful; proceeding");
                    } else {
                        Serial.println("[NVS REPAIR] Repair did not restore NVS blob writes");
                    }
                } else {
                    Serial.println("[NVS REPAIR] nvs_open failed after reinit");
                }
            }
        } else {
            Serial.println("[NVS REPAIR] Repair marker present; skipping erase to avoid data loss");
        }
    }

    // Write to SD only when the caller has NOT already written screens to SD.
    // When skip_screen_blobs=true, handle_save_gauges() already wrote screens.bin
    // and signalk_paths.txt atomically; a second write here doubles SD I/O and
    // blocks the loop for an extra ~200ms — the primary cause of post-save sluggishness.
    if (!skip_screen_blobs) {
        Serial.println(any_nvs_ok ? "[SD SAVE] NVS OK; also writing to SD for redundancy"
                                   : "[SD SAVE] NVS blob writes failed; saving to SD as fallback");
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        // Batch write: all screens in one file — 3 FAT ops instead of 15
        {
            size_t total = sizeof(ScreenConfig) * NUM_SCREENS;
            File f = SD_MMC.open("/config/screens.bin.tmp", FILE_WRITE);
            if (!f) {
                Serial.println("[SD SAVE] Failed to open /config/screens.bin.tmp");
            } else {
                size_t written = f.write((const uint8_t *)screen_configs, total);
                f.flush();
                f.close();
                if (written == total) {
                    SD_MMC.remove("/config/screens.bin");
                    SD_MMC.rename("/config/screens.bin.tmp", "/config/screens.bin");
                    Serial.printf("[SD SAVE] Wrote /config/screens.bin -> %u bytes\n", (unsigned)written);
                } else {
                    SD_MMC.remove("/config/screens.bin.tmp");
                    Serial.printf("[SD SAVE] Short write /config/screens.bin -> %u/%u B, original preserved\n",
                                  (unsigned)written, (unsigned)total);
                }
            }
        } // end batch write
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        File spf = SD_MMC.open("/config/signalk_paths.txt", FILE_WRITE);
        if (spf) {
            for (int i = 0; i < NUM_SCREENS * 2; ++i) {
                spf.println(signalk_paths[i]);
            }
            spf.flush();
            spf.close();
        } else {
            Serial.println("[SD SAVE] Failed to open /config/signalk_paths.txt for writing");
        }
    }
} // end save_preferences

// Load preferences and screen configs from NVS or SD fallback
void load_preferences() {
    // Load settings (WiFi, Signalk) from SETTINGS_NAMESPACE
    preferences.end();
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
        saved_ssid = preferences.getString("ssid", "");
        saved_password = preferences.getString("password", "");
        saved_signalk_ip = preferences.getString("signalk_ip", "openplotter.local");
        saved_signalk_port = preferences.getUShort("signalk_port", 0);
        saved_hostname = preferences.getString("hostname", "");
        saved_own_mmsi = preferences.getString("own_mmsi", "");
        // Load auto-scroll interval (seconds)
        auto_scroll_sec = preferences.getUShort("auto_scroll", 0);
        unit_system = (UnitSystem)preferences.getUShort("unit_system", (uint16_t)UNIT_NAUTICAL_METRIC);
        screen_off_timeout_min = preferences.getUShort("screen_off_tmout", 0);
        // Load device settings
        buzzer_mode = (int)preferences.getUShort("buzzer_mode", (uint16_t)buzzer_mode);
        buzzer_cooldown_sec = preferences.getUShort("buzzer_cooldown", buzzer_cooldown_sec);
        // Arm cooldown timer so the first alarm fires after one cooldown period,
        // preventing spurious alarm from 0.00 before real SK data arrives.
        first_run_buzzer = false;
        last_buzzer_time = millis();
        uint16_t saved_brightness = preferences.getUShort("brightness", (uint16_t)LCD_Backlight);
        LCD_Backlight = (uint8_t)saved_brightness;
        // Apply brightness to hardware
        extern void Set_Backlight(uint8_t Light);
        Set_Backlight(LCD_Backlight);
            Serial.printf("[DEVICE SAVE] buzzer_mode=%d buzzer_cooldown_sec=%u first_run_buzzer=%d\n", buzzer_mode, buzzer_cooldown_sec, (int)first_run_buzzer);
        for (int i = 0; i < NUM_SCREENS * 2; ++i) {
            String key = String("skpath_") + i;
            signalk_paths[i] = preferences.getString(key.c_str(), "");
        }
        preferences.end();
    }
    // Fill in any missing SignalK paths from SD fallback file (per-path, not all-or-nothing)
    {
        const char *spfpath = "/config/signalk_paths.txt";
        if (SD_MMC.exists(spfpath)) {
            File spf = SD_MMC.open(spfpath, FILE_READ);
            if (spf) {
                int idx = 0;
                while (spf.available() && idx < NUM_SCREENS * 2) {
                    String line = spf.readStringUntil('\n');
                    line.trim();
                    if (signalk_paths[idx].length() == 0 && line.length() > 0) {
                        signalk_paths[idx] = line;
                        Serial.printf("[SD LOAD] Restored signalk_paths[%d] = '%s' from SD\n", idx, line.c_str());
                    }
                    idx++;
                }
                spf.close();
            }
        }
    }
    // Load custom display labels from SD (non-critical, silently skip if absent)
    {
        const char *lblpath = "/config/screen_labels.txt";
        if (SD_MMC.exists(lblpath)) {
            File lf = SD_MMC.open(lblpath, FILE_READ);
            if (lf) {
                int idx = 0;
                while (lf.available() && idx < NUM_SCREENS * 4) {
                    String line = lf.readStringUntil('\n');
                    line.trim();
                    g_custom_labels[idx] = line;
                    idx++;
                }
                lf.close();
                Serial.println("[SD LOAD] Loaded /config/screen_labels.txt");
            }
        }
    }
    Serial.printf("[DEBUG] Loaded settings: ssid='%s' signalk_ip='%s' port=%u\n",
                  saved_ssid.c_str(), saved_signalk_ip.c_str(), saved_signalk_port);

    // Initialize defaults
    for (int s = 0; s < NUM_SCREENS; ++s) {
        // zero screen_configs so defaults are predictable
        memset(&screen_configs[s], 0, sizeof(ScreenConfig));
        // sensible defaults for icon positions: top icon -> top (0), bottom icon -> bottom (2)
        screen_configs[s].icon_pos[0] = 0;
        screen_configs[s].icon_pos[1] = 2;
        // default to showing bottom gauge
        screen_configs[s].show_bottom = 1;
        // default to gauge display type
        screen_configs[s].display_type = DISPLAY_TYPE_GAUGE;
        // number display defaults (background uses bg_image field: empty/"Default" = default, bin path = file, "Custom Color" = color)
        strncpy(screen_configs[s].number_bg_color, "#000000", 7);
        screen_configs[s].number_font_size = 2;  // Large (96pt)
        strncpy(screen_configs[s].number_font_color, "#FFFFFF", 7);
        screen_configs[s].number_path[0] = '\0';  // Empty path
        // dual display defaults
        screen_configs[s].dual_top_path[0] = '\0';
        screen_configs[s].dual_top_font_size = 1;  // Medium (48pt)
        strncpy(screen_configs[s].dual_top_font_color, "#FFFFFF", 7);
        screen_configs[s].dual_bottom_path[0] = '\0';
        screen_configs[s].dual_bottom_font_size = 1;  // Medium (48pt)
        strncpy(screen_configs[s].dual_bottom_font_color, "#FFFFFF", 7);
        // quad display defaults
        screen_configs[s].quad_tl_path[0] = '\0';
        screen_configs[s].quad_tl_font_size = 0;  // Small (48pt)
        strncpy(screen_configs[s].quad_tl_font_color, "#FFFFFF", 7);
        screen_configs[s].quad_tr_path[0] = '\0';
        screen_configs[s].quad_tr_font_size = 0;  // Small (48pt)
        strncpy(screen_configs[s].quad_tr_font_color, "#FFFFFF", 7);
        screen_configs[s].quad_bl_path[0] = '\0';
        screen_configs[s].quad_bl_font_size = 0;  // Small (48pt)
        strncpy(screen_configs[s].quad_bl_font_color, "#FFFFFF", 7);
        screen_configs[s].quad_br_path[0] = '\0';
        screen_configs[s].quad_br_font_size = 0;  // Small (48pt)
        strncpy(screen_configs[s].quad_br_font_color, "#FFFFFF", 7);
        // gauge+number display defaults
        screen_configs[s].gauge_num_center_path[0] = '\0';
        screen_configs[s].gauge_num_center_font_size = 1;  // Medium (72pt)
        strncpy(screen_configs[s].gauge_num_center_font_color, "#FFFFFF", 7);
    }

    // Try to load screen configs from NVS (PREF_NAMESPACE)
    nvs_handle_t nvs_handle;
    esp_err_t nvs_err = nvs_open(PREF_NAMESPACE, NVS_READONLY, &nvs_handle);
    const size_t CHUNK_SIZE = 128;
    if (nvs_err == ESP_OK) {
        for (int s = 0; s < NUM_SCREENS; ++s) {
            char key[32];
            snprintf(key, sizeof(key), "screen%d", s);
            ScreenConfig tmp;
            size_t required = sizeof(ScreenConfig);
            esp_err_t err = nvs_get_blob(nvs_handle, key, &tmp, &required);
            if (err == ESP_OK && required == sizeof(ScreenConfig)) {
                memcpy(&screen_configs[s], &tmp, sizeof(ScreenConfig));
                continue;
            }
            // try chunked parts
            int parts = (sizeof(ScreenConfig) + CHUNK_SIZE - 1) / CHUNK_SIZE;
            bool got_parts = true;
            for (int part = 0; part < parts; ++part) {
                snprintf(key, sizeof(key), "screen%d.part%d", s, part);
                size_t part_sz = ((part + 1) * CHUNK_SIZE > sizeof(ScreenConfig)) ? (sizeof(ScreenConfig) - part * CHUNK_SIZE) : CHUNK_SIZE;
                esp_err_t perr = nvs_get_blob(nvs_handle, key, ((uint8_t *)&screen_configs[s]) + part * CHUNK_SIZE, &part_sz);
                if (perr != ESP_OK) {
                    got_parts = false;
                    break;
                }
            }
            if (got_parts) continue;
        }
        nvs_close(nvs_handle);
    }

    // Always prefer SD over NVS — SD is the authoritative save target.
    // NVS is only a fallback when SD is unavailable.
    bool restored_from_sd = false;

    // Try batch file first (written by current firmware)
    if (SD_MMC.exists("/config/screens.bin")) {
        File f = SD_MMC.open("/config/screens.bin", FILE_READ);
        if (f) {
            size_t total = sizeof(ScreenConfig) * NUM_SCREENS;
            if (f.size() != total) {
                Serial.printf("[SD LOAD] File size mismatch: got %u, expected %u, skipping batch read\n",
                              (unsigned)f.size(), (unsigned)total);
                f.close();
            } else {
                size_t got = f.read((uint8_t *)screen_configs, total);
                f.close();
                Serial.printf("[SD LOAD] Read /config/screens.bin -> %u bytes (expected %u)\n", (unsigned)got, (unsigned)total);
                if (got == total) {
                    for (int s = 0; s < NUM_SCREENS; ++s) {
                        bool valid = true;
                        for (int g = 0; g < 2 && valid; ++g) {
                            for (int p = 0; p < 5; ++p) {
                                if (screen_configs[s].cal[g][p].angle < -360 || screen_configs[s].cal[g][p].angle > 360) {
                                    valid = false; break;
                                }
                            }
                        }
                        if (!valid) {
                            Serial.printf("[CONFIG ERROR] SD batch config for screen %d invalid, restoring defaults\n", s);
                            memset(&screen_configs[s], 0, sizeof(ScreenConfig));
                        } else {
                            restored_from_sd = true;
                        }
                    }
                }
            } // end size check
        } else {
            Serial.println("[SD LOAD] Failed to open /config/screens.bin");
        }
    }

    // Backward compat: fall back to per-screen files if batch file absent
    if (!restored_from_sd) {
        for (int s = 0; s < NUM_SCREENS; ++s) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s);
            if (SD_MMC.exists(sdpath)) {
                File f = SD_MMC.open(sdpath, FILE_READ);
                if (f) {
                    size_t got = f.read((uint8_t *)&screen_configs[s], sizeof(ScreenConfig));
                    Serial.printf("[SD LOAD] Read '%s' -> %u bytes (expected %u)\n", sdpath, (unsigned)got, (unsigned)sizeof(ScreenConfig));
                    f.close();
                    bool valid = true;
                    for (int g = 0; g < 2 && valid; ++g) {
                        for (int p = 0; p < 5; ++p) {
                            if (screen_configs[s].cal[g][p].angle < -360 || screen_configs[s].cal[g][p].angle > 360) {
                                valid = false; break;
                            }
                        }
                    }
                    if (!valid) {
                        Serial.printf("[CONFIG ERROR] SD config for screen %d invalid, restoring defaults\n", s);
                        memset(&screen_configs[s], 0, sizeof(ScreenConfig));
                    } else {
                        restored_from_sd = true;
                    }
                } else {
                    Serial.printf("[SD LOAD] Failed to open '%s', keeping NVS data for screen %d\n", sdpath, s);
                }
            } else {
                Serial.printf("[SD LOAD] No SD config for screen %d, keeping NVS data\n", s);
            }
        }
    }

    if (restored_from_sd) {
        Serial.println("[CONFIG RESTORE] Screen configs restored from SD after NVS was blank/default.");
    }

    // Copy loaded calibration into gauge_cal for runtime use
    // Debug: dump raw screen_configs contents (strings + small hex preview) to help diagnose missing icon paths
    for (int si = 0; si < NUM_SCREENS; ++si) {
        ESP_LOGI(TAG_SETUP, "[DUMP SC] Screen %d: icon_top='%s' icon_bottom='%s'", si,
                 screen_configs[si].icon_paths[0], screen_configs[si].icon_paths[1]);
        // Print small hex preview of the first 64 bytes
        const uint8_t *bytes = (const uint8_t *)&screen_configs[si];
        char hbuf[3*17];
        for (int i = 0; i < 16; ++i) {
            snprintf(&hbuf[i*3], 4, "%02X ", bytes[i]);
        }
        hbuf[16*3-1] = '\0';
        ESP_LOGD(TAG_SETUP, "[DUMP SC] raw[0..15]=%s", hbuf);
    }
    for (int s = 0; s < NUM_SCREENS; ++s) {
        for (int g = 0; g < 2; ++g) {
            for (int p = 0; p < 5; ++p) {
                gauge_cal[s][g][p] = screen_configs[s].cal[g][p];
            }
        }
    }

    // No automatic default icon set; keep blank unless user selects one via UI
}

// Forward declaration — generates a single screen's config HTML fragment
static void stream_screen_config(int s);

void handle_gauges_page() {
    // Lightweight shell page — per-screen config is loaded on demand via AJAX
    // from /gauges/screen?s=N.  This keeps peak HTML generation under ~4 KB,
    // preventing the OOM / TCP-buffer crashes that occurred when building all
    // 5 screens' config in a single chunked response.
    skip_next_load_preferences = false;
    g_config_page_last_seen = millis();

    // Pause WS immediately — if the WS is connected and streaming data its
    // TCP receive buffers consume ~10 KB of iRAM.  Without pausing here the
    // shell response alone can drop iRAM below the threshold needed for the
    // subsequent AJAX fragment fetches, causing a crash.
    pause_signalk_ws();
    pause_n2k_updates();

    Serial.printf("[GAUGES] shell handler, iRAM=%u\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    extern bool test_mode;

    esp_task_wdt_reset();
    config_server.sendHeader("Connection", "close");
    config_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    config_server.send(200, "text/html; charset=utf-8", "");

    // Re-use the SAME static String as handle_gauges_screen() to avoid
    // holding two 4096-byte buffers permanently in iRAM.
    extern String g_http_html_buf;
    extern bool   g_http_html_buf_reserved;
    String& html = g_http_html_buf;
    if (!g_http_html_buf_reserved) {
        html.reserve(8192);  // >4096 → PSRAM via SPIRAM threshold
        g_http_html_buf_reserved = true;
    }
    html.clear();
    auto flushHtml = [&]() {
        if (html.length() > 0) {
            esp_task_wdt_reset();
            config_server.sendContent(html);
            html.clear();
            // Yield 20 ms so the lwIP task (priority 18) can process incoming
            // TCP ACKs and free send-buffer pbufs before we queue more data.
            // Without this, rapid sendContent() calls accumulate unACKed pbufs
            // in iRAM and exhaust it mid-fragment.
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    };

    // ── HTML head + CSS ──────────────────────────────────────────────
    html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += STYLE;
    html += "<title>Gauge Calibration</title></head><body><div class='container'>";
    html += "<h2>Gauge Calibration</h2>";

    // Test mode toggle — AJAX, no full page reload
    html += "<div style='margin-bottom:16px;text-align:center;'>";
    html += "<button type='button' id='testModeBtn' onclick='toggleTestMode()' style='padding:8px 16px;font-size:1em;'>";
    html += (test_mode ? "Disable Test Mode" : "Enable Test Mode");
    html += "</button> ";
    html += "<span id='testModeLabel' style='font-weight:bold;color:";
    html += (test_mode ? "#388e3c;'>TEST MODE ON" : "#b71c1c;'>TEST MODE OFF");
    html += "</span></div>";
    // Form wrapper — screen content is injected inside via AJAX
    html += "<form id='calibrationForm' method='POST' action='/save-gauges' accept-charset='utf-8'>";
    html += "<input type='hidden' name='save_screen' id='save_screen' value='0'>";

    // Tab bar
    html += "<div style='margin-bottom:16px; text-align:center;'>";
    for (int s = 0; s < NUM_SCREENS; ++s) {
        html += "<button type='button' class='tab-btn' id='tabbtn_" + String(s) +
                "' onclick='showScreenTab(" + String(s) +
                ")' style='margin:0 4px;padding:8px 16px;font-size:1em;'>Screen " +
                String(s + 1) + "</button>";
    }
    html += "</div>";

    // Placeholder — filled by showScreenTab() AJAX
    html += "<div id='screen-content' style='min-height:200px;'>";
    html += "<p style='text-align:center;color:#888;padding:40px 0;'>Loading screen configuration...</p>";
    html += "</div>";

    // Apply button
    html += "<div style='text-align:center;margin-top:16px;'>";
    html += "<input type='button' id='saveBtn' value='Apply (no reboot)' onclick='ajaxSave()' style='padding:10px 24px;font-size:1.1em;'>";
    html += "</div>";
    html += "</form>";
    html += "<p style='text-align:center;'><a href='/'>Back</a></p>";
    flushHtml();
    // ── JavaScript — AJAX tab loader, save, toggle ─────────────────
    html += "<script>\n";
    html += "var NUM_SCREENS=" + String(NUM_SCREENS) + ";\n";
    html += "var currentTab=-1;\n";

    // ── N2K dropdown options — built once here, applied by populateN2kSelects() ──
    // Fragments now emit empty <select class="n2k-sel" data-val="..."> elements.
    // populateN2kSelects() fills them after each fragment is injected into the DOM,
    // eliminating 3-12 KB of repeated <option> markup from every fragment response.
    html += "var N2K_OPTS_HTML='<option value=\"0\">-- None --</option>";
    for (int i = 0; i < n2k_all_fields_count; ++i) {
        N2kField f = n2k_all_fields[i];
        html += "<option value=\""; html += (int)f; html += "\">";
        html += n2k_field_label(f); html += "</option>";
    }
    html += "<option value=\"__custom__\">Custom path...</option>';\n";

    // Populate all .n2k-sel selects within a given DOM root element.
    // Reads the data-val attribute to set the correct initial selection,
    // and handles the custom-path case (no matching enum value found).
    html += "function populateN2kSelects(root){\n";
    html += "  var sels=(root||document).querySelectorAll('.n2k-sel');\n";
    html += "  for(var i=0;i<sels.length;i++){\n";
    html += "    var sel=sels[i];\n";
    html += "    sel.innerHTML=N2K_OPTS_HTML;\n";
    html += "    var val=sel.getAttribute('data-val')||'0';\n";
    html += "    var found=false;\n";
    html += "    for(var j=0;j<sel.options.length;j++){if(sel.options[j].value===val){sel.selectedIndex=j;found=true;break;}}\n";
    html += "    var hid=document.getElementById(sel.id.replace(/_s$/,'_v'));\n";
    html += "    var txt=document.getElementById(sel.id.replace(/_s$/,'_t'));\n";
    html += "    if(!found&&val!=='0'&&val!==''){\n";
    html += "      sel.value='__custom__';\n";
    html += "      if(txt){txt.style.display='inline-block';txt.value=val;}\n";
    html += "      if(hid)hid.value=val;\n";
    html += "    }\n";
    html += "  }\n";
    html += "}\n";

    // ── preload generation counter — incremented on every deliberate tab click ──
    // Causes any in-flight background preload loop to abort, so the user's
    // explicit tab request is not queued behind 4 preload fetches.
    html += "var preloadGen=0;\n";

    // showScreenTab: switch display + load fragment for the chosen tab
    html += "function showScreenTab(idx){\n";
    html += "  if(idx===currentTab) return;\n";
    html += "  ++preloadGen;\n";  // cancel any running background preload
    html += "  currentTab=idx;\n";
    html += "  document.getElementById('save_screen').value=idx;\n";
    html += "  for(var s=0;s<NUM_SCREENS;s++){\n";
    html += "    var b=document.getElementById('tabbtn_'+s);\n";
    html += "    if(b) b.style.background=(s===idx?'#e3eaf6':'#f4f6fa');\n";
    html += "  }\n";
    // Always switch the physical display, even when fragment is cached
    html += "  fetch('/gauges/switch?s='+idx).catch(function(){});\n";
    html += "  loadScreenFragment(idx,-1);\n";
    html += "}\n";

    // Fragment cache: key = "idx" for stored type, "idx_type" for type override.
    // Re-visiting the same tab is instant; first visit fetches from device.
    html += "var fragmentCache={};\n";
    html += "function loadScreenFragment(idx,typeOverride,retries){\n";
    html += "  retries=retries||0;\n";
    html += "  var cont=document.getElementById('screen-content');\n";
    html += "  var ckey=(typeOverride>=0)?(idx+'_'+typeOverride):String(idx);\n";
    html += "  if(fragmentCache[ckey]){\n";
    html += "    cont.innerHTML=fragmentCache[ckey];\n";
    html += "    populateN2kSelects(cont);\n";  // re-populate dropdowns from cache hit
    html += "    return;\n";
    html += "  }\n";
    html += "  cont.innerHTML='<p style=\"text-align:center;color:#888;padding:40px 0;\">Loading...</p>';\n";
    html += "  var url='/gauges/screen?s='+idx;\n";
    html += "  if(typeOverride>=0)url+='&type='+typeOverride;\n";
    html += "  fetch(url)\n";
    html += "    .then(function(r){return r.text();})\n";
    html += "    .then(function(h){\n";
    html += "      fragmentCache[ckey]=h;\n";
    html += "      cont.innerHTML=h;\n";
    html += "      populateN2kSelects(cont);\n";  // populate dropdowns on fresh load
    html += "      preloadFragments(idx);\n";
    html += "    })\n";
    // Retry up to 3 times on network error (e.g. server still closing shell connection)
    html += "    .catch(function(e){\n";
    html += "      if(retries<3){setTimeout(function(){loadScreenFragment(idx,typeOverride,retries+1);},700);}\n";
    html += "      else{cont.innerHTML='<p style=\"color:red;text-align:center;\">Failed &ndash; '+e+'</p>';}\n";
    html += "    });\n";
    html += "}\n";
    // Preload remaining screen fragments sequentially in the background.
    // Uses a generation counter — if the user clicks a tab (incrementing preloadGen)
    // the active preload loop detects the stale generation and stops immediately.
    html += "function preloadFragments(skipIdx){\n";
    html += "  var gen=preloadGen;\n";
    html += "  var order=[];\n";
    html += "  for(var s=0;s<NUM_SCREENS;s++){if(s!==skipIdx&&!fragmentCache[String(s)])order.push(s);}\n";
    html += "  var i=0;\n";
    html += "  function next(){\n";
    html += "    if(gen!==preloadGen||i>=order.length)return;\n";  // abort if user clicked a tab
    html += "    var si=order[i++];\n";
    html += "    fetch('/gauges/screen?s='+si)\n";
    html += "      .then(function(r){return r.text();})\n";
    html += "      .then(function(h){if(!fragmentCache[String(si)])fragmentCache[String(si)]=h;setTimeout(next,600);})\n";
    html += "      .catch(function(){setTimeout(next,800);});\n";
    html += "  }\n";
    html += "  setTimeout(next,600);\n";  // wait 600ms after first fragment before preloading
    html += "}\n";

    // toggleBgImageColor: show/hide the inline Custom Color picker in the fragment
    html += "function toggleBgImageColor(screen){\n";
    html += "  var sel=document.getElementById('bg_image_'+screen);\n";
    html += "  ['number_bg_color_div_','dual_bg_color_div_','graph_bg_color_div_','pos_bg_color_div_'].forEach(function(p){\n";
    html += "    var d=document.getElementById(p+screen);\n";
    html += "    if(sel&&d)d.style.display=(sel.value==='Custom Color'?'block':'none');\n";
    html += "  });\n";
    html += "}\n";

    // initScreenTab: no-op — fragment is now type-specific, no client-side toggle needed
    html += "function initScreenTab(s){};\n";

    // toggleGaugeConfig — refetch fragment with new display type
    html += "function toggleGaugeConfig(screen){"
            "var sel=document.getElementById('displaytype_'+screen);"
            "if(!sel)return;"
            "loadScreenFragment(screen,parseInt(sel.value,10));"
            "}\n";

    // AJAX save — POST only the visible screen's fields
    html += "function ajaxSave(){\n";
    html += "  var btn=document.getElementById('saveBtn');\n";
    html += "  if(btn){btn.disabled=true;btn.value='Saving...';}\n";
    html += "  var form=document.getElementById('calibrationForm');\n";
    html += "  var params=new URLSearchParams(new FormData(form)).toString();\n";
    html += "  fetch('/save-gauges',{method:'POST',\n";
    html += "    headers:{'Content-Type':'application/x-www-form-urlencoded'},\n";
    html += "    body:params})\n";
    html += "  .then(function(r){return r.json();})\n";
    html += "  .then(function(j){\n";
    html += "    if(btn){btn.disabled=false;btn.value='Saved!';\n";
    html += "    setTimeout(function(){btn.value='Apply (no reboot)';},2000);}\n";
    // Invalidate cache for the saved screen so next tab visit reflects saved values
    html += "    var ct=currentTab;\n";
    html += "    for(var k in fragmentCache){if(k===String(ct)||k.indexOf(ct+'_')===0)delete fragmentCache[k];}\n";
    html += "  })\n";
    html += "  .catch(function(e){\n";
    html += "    console.error('ajaxSave error',e);\n";
    html += "    if(btn){btn.disabled=false;btn.value='Error - retry';}\n";
    html += "  });\n";
    html += "}\n";

    // Toggle test mode via AJAX
    html += "function toggleTestMode(){\n";
    html += "  fetch('/toggle-test-mode',{method:'POST'})\n";
    html += "  .then(function(r){return r.json();})\n";
    html += "  .then(function(j){\n";
    html += "    var btn=document.getElementById('testModeBtn');\n";
    html += "    var lbl=document.getElementById('testModeLabel');\n";
    html += "    if(j.test_mode){\n";
    html += "      if(btn)btn.textContent='Disable Test Mode';\n";
    html += "      if(lbl){lbl.style.color='#388e3c';lbl.textContent='TEST MODE ON';}\n";
    html += "    } else {\n";
    html += "      if(btn)btn.textContent='Enable Test Mode';\n";
    html += "      if(lbl){lbl.style.color='#b71c1c';lbl.textContent='TEST MODE OFF';}\n";
    html += "    }\n";
    html += "    var testBtns=document.querySelectorAll('button[onclick^=\"testGaugePoint\"]');\n";
    html += "    for(var i=0;i<testBtns.length;i++){\n";
    html += "      testBtns[i].disabled = !j.test_mode;\n";
    html += "      testBtns[i].style.backgroundColor = j.test_mode ? '#4a90e2' : '#cccccc';\n";
    html += "      testBtns[i].style.cursor = j.test_mode ? 'pointer' : 'not-allowed';\n";
    html += "    }\n";
    html += "  }).catch(function(e){console.error(e);});\n";
    html += "}\n";

    // Test gauge point via AJAX (used by buttons inside screen fragment)
    html += "function testGaugePoint(s,g,p){\n";
    html += "  var ai=document.querySelector('input[name=\"angle_'+s+'_'+g+'_'+p+'\"]');\n";
    html += "  var angle=ai?ai.value:'0';\n";
    html += "  fetch('/test-gauge',{method:'POST',\n";
    html += "    headers:{'Content-Type':'application/x-www-form-urlencoded'},\n";
    html += "    body:'screen='+s+'&gauge='+g+'&point='+p+'&angle='+angle})\n";
    html += "  .catch(function(e){console.error(e);});\n";
    html += "}\n";

    // Keepalive: ping every 8s so the idle watchdog doesn't resume prematurely
    html += "setInterval(function(){fetch('/gauges/ping').catch(function(){});},8000);\n";
    // Immediate resume when user navigates away or closes the page
    html += "window.addEventListener('pagehide',function(){navigator.sendBeacon('/gauges/close');});\n";
    html += "window.addEventListener('beforeunload',function(){navigator.sendBeacon('/gauges/close');});\n";

    // Load first tab on page load
    html += "document.addEventListener('DOMContentLoaded',function(){\n";
    html += "  var initial=0;\n";
    html += "  if(location.hash&&location.hash.indexOf('#tab')===0) initial=parseInt(location.hash.replace('#tab',''))||0;\n";
    html += "  showScreenTab(initial);\n";
    html += "});\n";
    html += "</script>\n";
    html += "</div></body></html>";
    flushHtml();

    esp_task_wdt_reset();
    config_server.sendContent(""); // chunked transfer terminator
    // Let lwIP transmit the queued data before sending RST, then close
    // immediately to free PCB + pbufs.  Without this the shell page's
    // ~4 KB of send-buffer pbufs stay allocated until the browser ACKs
    // them (several main-loop iterations later), fragmenting iRAM just
    // before the AJAX fragment handler runs.
    vTaskDelay(pdMS_TO_TICKS(30));
    rst_close_client();
    Serial.printf("[GAUGES] shell complete, iRAM=%u\n",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// ═══════════════════════════════════════════════════════════════════════════
// handle_gauges_screen — AJAX endpoint returning HTML fragment for ONE screen
// ═══════════════════════════════════════════════════════════════════════════
void handle_gauges_screen() {
    int s = config_server.arg("s").toInt();
    if (s < 0 || s >= NUM_SCREENS) {
        config_server.send(400, "text/plain", "Bad screen index");
        return;
    }
    // Keep WS paused while config page is open — don't resume/re-pause
    // per fragment, that thrashes TCP buffers and wastes iRAM.
    g_config_page_last_seen = millis();
    pause_signalk_ws();           // no-op if already paused
    pause_n2k_updates();          // no-op if already paused

    // NOTE: ui_set_screen() is called AFTER the HTTP response completes
    // (see bottom of this function) to avoid LVGL DMA flushes during TCP sends.

    extern bool test_mode;
    const std::vector<String>& iconFiles = g_iconFiles;
    const std::vector<String>& bgFiles   = g_bgFiles;

    // displayType: use ?type=N override (user changing dropdown) or the stored value
    int displayType = screen_configs[s].display_type;
    if (config_server.hasArg("type")) {
        int t = config_server.arg("type").toInt();
        if (t >= 0 && t <= 10) displayType = t;
    }

    Serial.printf("[GAUGES] fragment s=%d type=%d iRAM=%u\n", s, displayType,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Build the ENTIRE fragment in the PSRAM-backed String, then send once
    // with Content-Length.  Sending many small chunks via sendContent() queues
    // separate lwIP pbufs (each ~1460 B in iRAM) faster than the browser ACKs
    // them, exhausting iRAM mid-fragment.  One large send lets the TCP stack
    // manage flow control internally within a bounded send window.
    extern String g_http_html_buf;
    extern bool   g_http_html_buf_reserved;
    String& html = g_http_html_buf;
    if (!g_http_html_buf_reserved) {
        html.reserve(8192);  // >4096 → PSRAM via SPIRAM threshold
        g_http_html_buf_reserved = true;
    }
    html.clear();
    // flushHtml is a no-op here — we accumulate everything in PSRAM.
    // The esp_task_wdt_reset() keeps the watchdog happy during long builds.
    auto flushHtml = [&]() { esp_task_wdt_reset(); };
    html += "<h3>Screen " + String(s+1) + "</h3>";

    // Display Type dropdown
    html += "<div style='margin-bottom:16px;'><label>Display Type: <select name='displaytype_" + String(s) + "' id='displaytype_" + String(s) + "' onchange='toggleGaugeConfig(" + String(s) + ")'>";
    const char* dtNames[] = {"Gauge","Number","Dual","Quad","Gauge + Number","Graph","Compass","Position","AIS","Attitude","Anchor Alarm"};
    for (int dt = 0; dt < 11; ++dt) {
        html += "<option value='" + String(dt) + "'";
        if (displayType == dt) html += " selected";
        html += ">" + String(dtNames[dt]) + "</option>";
    }
    html += "</select></label></div>";

    // Background selection — not applicable for AIS/Attitude/Anchor
    if (displayType < 8) {
    String savedBg = String(screen_configs[s].background_path);
    String savedBgNorm = savedBg; savedBgNorm.toLowerCase();
    savedBgNorm.replace("S://", "S:/");
    while (savedBgNorm.indexOf("//") != -1) savedBgNorm.replace("//", "/");
    html += "<div style='margin-bottom:8px;'><label>Background: <select name='bg_" + String(s) + "' id='bg_image_" + String(s) + "' onchange='toggleBgImageColor(" + String(s) + ")'>";
    html += "<option value=''";
    if (savedBg.length() == 0) html += " selected='selected'";
    html += ">Default</option>";
    for (const auto& b : bgFiles) {
        String iconNorm = b; iconNorm.toLowerCase();
        iconNorm.replace("S://", "S:/");
        while (iconNorm.indexOf("//") != -1) iconNorm.replace("//", "/");
        html += "<option value='" + b + "'";
        if (iconNorm == savedBgNorm && savedBg.length() > 0) html += " selected='selected'";
        html += ">" + b + "</option>";
    }
    html += "<option value='Custom Color'";
    if (savedBg == "Custom Color") html += " selected='selected'";
    if (displayType == DISPLAY_TYPE_GAUGE ||
        displayType == DISPLAY_TYPE_GAUGE_NUMBER) html += " hidden disabled";
    html += ">Custom Color</option>";
    html += "</select></label></div>";
    flushHtml();
    } // end if (displayType < 8) — background row

    bool isCustomColor = (String(screen_configs[s].background_path) == "Custom Color");

    // ── Number display config ────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_NUMBER) {
    html += "<h4>Number Display Settings</h4>";
    flushHtml();
    html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "number_path_" + String(s), screen_configs[s].number_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:8px;'><label>Display Label (override): "
            "<input type='text' name='clabel_" + String(s) + "_0' placeholder='e.g. Depth' value='";
    html += g_custom_labels[s * 4 + 0];
    html += "' style='width:40%;'></label> "
            "<small style='color:#888'>Leave blank to use default field name</small></div>";
    html += "<div id='number_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColor ? "block" : "none") + ";'>";
    html += "<label>Background Color: <input name='number_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='number_font_size_" + String(s) + "'>";
    html += "<option value='2'" + String(screen_configs[s].number_font_size <= 2 ? " selected" : "") + ">Large (96pt)</option>";
    html += "<option value='3'" + String(screen_configs[s].number_font_size == 3 ? " selected" : "") + ">X-Large (120pt)</option>";
    html += "<option value='4'" + String(screen_configs[s].number_font_size == 4 ? " selected" : "") + ">XX-Large (144pt)</option>";
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='number_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_font_color[0] ? screen_configs[s].number_font_color : "#FFFFFF") + "'></label></div>";
    // Number alarms
    html += "<div class='icon-section'><h5 style='margin:0 0 8px;'>Alarms</h5>";
    html += "<div style='display:flex;gap:16px;flex-wrap:wrap;'>";
    html += "<div><label>Low Alarm &lt; <input name='num_low_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].min[0][1]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='num_low_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[0][1]) html += " checked";
    html += "> Enable</label></div>";
    html += "<div><label>High Alarm &gt; <input name='num_high_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].max[0][2]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='num_high_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[0][2]) html += " checked";
    html += "> Enable</label></div></div></div>";
    } // end displayType == DISPLAY_TYPE_NUMBER
    flushHtml();

    // ── Compass config ───────────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_COMPASS) {
    html += "<h4>Compass Settings</h4>";
    html += "<div style='margin-bottom:8px;'><label>Background Color: <input name='compass_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
    html += "<div style='margin-bottom:8px;'><em>Heading data from NMEA 2000 PGN 127250 (Vessel Heading)</em></div>";
    // Hidden field so save handler always gets a value for compass heading
    html += "<input type='hidden' name='compass_hdg_src_" + String(s) + "' value='" + String((int)N2K_HEADING) + "'>";
    // Compass extra data fields — use compass_bl_*/compass_br_* names to avoid
    // clashing with the identically-named inputs in the Quad config section.
    html += "<h4>Extra Data Fields</h4><div style='display:flex;gap:16px;flex-wrap:wrap;'>";
    // BL
    html += "<div style='flex:1;min-width:200px;'><h5>Bottom-Left</h5>";
    flushHtml();
    html += "<div style='margin-bottom:4px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "compass_bl_path_" + String(s), screen_configs[s].quad_bl_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:4px;'><label>Font Size: <select name='compass_bl_font_size_" + String(s) + "'>";
    html += "<option value='0'" + String(screen_configs[s].quad_bl_font_size == 0 ? " selected" : "") + ">Small (48pt)</option>";
    html += "<option value='1'" + String(screen_configs[s].quad_bl_font_size == 1 ? " selected" : "") + ">Medium (72pt)</option>";
    html += "<option value='2'" + String(screen_configs[s].quad_bl_font_size == 2 ? " selected" : "") + ">Large (96pt)</option>";
    html += "</select></label></div>";
    html += "<div style='margin-bottom:4px;'><label>Font Color: <input name='compass_bl_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].quad_bl_font_color[0] ? screen_configs[s].quad_bl_font_color : "#FFFFFF") + "'></label></div></div>";
    // BR
    html += "<div style='flex:1;min-width:200px;'><h5>Bottom-Right</h5>";
    flushHtml();
    html += "<div style='margin-bottom:4px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "compass_br_path_" + String(s), screen_configs[s].quad_br_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:4px;'><label>Font Size: <select name='compass_br_font_size_" + String(s) + "'>";
    html += "<option value='0'" + String(screen_configs[s].quad_br_font_size == 0 ? " selected" : "") + ">Small (48pt)</option>";
    html += "<option value='1'" + String(screen_configs[s].quad_br_font_size == 1 ? " selected" : "") + ">Medium (72pt)</option>";
    html += "<option value='2'" + String(screen_configs[s].quad_br_font_size == 2 ? " selected" : "") + ">Large (96pt)</option>";
    html += "</select></label></div>";
    html += "<div style='margin-bottom:4px;'><label>Font Color: <input name='compass_br_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].quad_br_font_color[0] ? screen_configs[s].quad_br_font_color : "#FFFFFF") + "'></label></div></div>";
    html += "</div></div>"; // end compass extra fields
    } // end displayType == DISPLAY_TYPE_COMPASS
    flushHtml();

    // ── Dual display config ──────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_DUAL) {
    html += "<h4>Dual Display Settings</h4>";
    html += "<div id='dual_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColor ? "block" : "none") + ";'>";
    html += "<label>Background Color: <input name='dual_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
    // Top
    html += "<h5>Top Display</h5>";
    flushHtml();
    html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "dual_top_path_" + String(s), screen_configs[s].dual_top_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:8px;'><label>Display Label (override): "
            "<input type='text' name='clabel_" + String(s) + "_0' placeholder='e.g. Speed' value='";
    html += g_custom_labels[s * 4 + 0];
    html += "' style='width:40%;'></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='dual_top_font_size_" + String(s) + "'>";
    for (int fs = 0; fs < 5; fs++) {
        const char* fsNames[] = {"Small (48pt)","Medium (72pt)","Large (96pt)","X-Large (120pt)","XX-Large (144pt)"};
        html += "<option value='" + String(fs) + "'";
        if (screen_configs[s].dual_top_font_size == fs) html += " selected";
        html += ">" + String(fsNames[fs]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='dual_top_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].dual_top_font_color[0] ? screen_configs[s].dual_top_font_color : "#FFFFFF") + "'></label></div>";
    // Top alarms
    html += "<div class='icon-section'><h5 style='margin:0 0 6px;'>Alarms</h5><div style='display:flex;gap:16px;flex-wrap:wrap;'>";
    html += "<label>Low &lt; <input name='dual_top_low_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].min[0][1]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='dual_top_low_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[0][1]) html += " checked";
    html += "> Enable</label> ";
    html += "<label>High &gt; <input name='dual_top_high_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].max[0][2]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='dual_top_high_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[0][2]) html += " checked";
    html += "> Enable</label></div></div>";
    flushHtml();
    // Bottom
    html += "<h5>Bottom Display</h5>";
    flushHtml();
    html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "dual_bottom_path_" + String(s), screen_configs[s].dual_bottom_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:8px;'><label>Display Label (override): "
            "<input type='text' name='clabel_" + String(s) + "_1' placeholder='e.g. Depth' value='";
    html += g_custom_labels[s * 4 + 1];
    html += "' style='width:40%;'></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='dual_bottom_font_size_" + String(s) + "'>";
    for (int fs = 0; fs < 5; fs++) {
        const char* fsNames[] = {"Small (48pt)","Medium (72pt)","Large (96pt)","X-Large (120pt)","XX-Large (144pt)"};
        html += "<option value='" + String(fs) + "'";
        if (screen_configs[s].dual_bottom_font_size == fs) html += " selected";
        html += ">" + String(fsNames[fs]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='dual_bottom_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].dual_bottom_font_color[0] ? screen_configs[s].dual_bottom_font_color : "#FFFFFF") + "'></label></div>";
    // Bottom alarms
    html += "<div class='icon-section'><h5 style='margin:0 0 6px;'>Alarms</h5><div style='display:flex;gap:16px;flex-wrap:wrap;'>";
    html += "<label>Low &lt; <input name='dual_bot_low_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].min[1][1]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='dual_bot_low_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[1][1]) html += " checked";
    html += "> Enable</label> ";
    html += "<label>High &gt; <input name='dual_bot_high_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].max[1][2]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='dual_bot_high_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[1][2]) html += " checked";
    html += "> Enable</label></div></div>";
    } // end displayType == DISPLAY_TYPE_DUAL
    flushHtml();

    // ── Quad display config ──────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_QUAD) {
    html += "<h4>Quad Display Settings</h4>";
    html += "<div style='margin-bottom:8px;'><label>Background Color: <input name='quad_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
    // Quad quadrant helper
    auto addQuadrantHTML = [&](const char* name, const char* label, char* path, uint8_t size, char* color, int g_alm, int zl, int zh, int lslot) {
        html += "<h5>" + String(label) + "</h5>";
        flushHtml();
        html += "<div style='margin-bottom:4px;'><label>NMEA 2000 Field: ";
        n2k_select(html, "quad_" + String(name) + "_path_" + String(s), path);
        html += "</label></div>";
        html += "<div style='margin-bottom:4px;'><label>Display Label (override): "
                "<input type='text' name='clabel_" + String(s) + "_" + String(lslot) + "' placeholder='e.g. Speed' value='";
        html += g_custom_labels[s * 4 + lslot];
        html += "' style='width:35%;'></label></div>";
        html += "<div style='margin-bottom:4px;'><label>Font Size: <select name='quad_" + String(name) + "_font_size_" + String(s) + "'>";
        for (int fs = 0; fs < 3; fs++) {
            const char* n[] = {"Small (48pt)","Medium (72pt)","Large (96pt)"};
            html += "<option value='" + String(fs) + "'";
            if (size == fs) html += " selected";
            html += ">" + String(n[fs]) + "</option>";
        }
        html += "</select></label></div>";
        html += "<div style='margin-bottom:4px;'><label>Font Color: <input name='quad_" + String(name) + "_font_color_" + String(s) + "' type='color' value='" + String(color[0] ? color : "#FFFFFF") + "'></label></div>";
        html += "<div class='icon-section'><h5 style='margin:0 0 6px;'>Alarms</h5><div style='display:flex;gap:8px;flex-wrap:wrap;'>";
        html += "<label>Low &lt; <input name='quad_" + String(name) + "_low_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].min[g_alm][zl]) + "' style='width:80px'></label> ";
        html += "<label><input type='checkbox' name='quad_" + String(name) + "_low_buz_" + String(s) + "'";
        if (screen_configs[s].buzzer[g_alm][zl]) html += " checked";
        html += "> Enable</label> ";
        html += "<label>High &gt; <input name='quad_" + String(name) + "_high_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].max[g_alm][zh]) + "' style='width:80px'></label> ";
        html += "<label><input type='checkbox' name='quad_" + String(name) + "_high_buz_" + String(s) + "'";
        if (screen_configs[s].buzzer[g_alm][zh]) html += " checked";
        html += "> Enable</label></div></div>";
    };
    addQuadrantHTML("tl", "Top-Left",     screen_configs[s].quad_tl_path, screen_configs[s].quad_tl_font_size, screen_configs[s].quad_tl_font_color, 0, 1, 2, 0);
    flushHtml();
    addQuadrantHTML("tr", "Top-Right",    screen_configs[s].quad_tr_path, screen_configs[s].quad_tr_font_size, screen_configs[s].quad_tr_font_color, 0, 3, 4, 1);
    flushHtml();
    addQuadrantHTML("bl", "Bottom-Left",  screen_configs[s].quad_bl_path, screen_configs[s].quad_bl_font_size, screen_configs[s].quad_bl_font_color, 1, 1, 2, 2);
    flushHtml();
    addQuadrantHTML("br", "Bottom-Right", screen_configs[s].quad_br_path, screen_configs[s].quad_br_font_size, screen_configs[s].quad_br_font_color, 1, 3, 4, 3);
    flushHtml();
    } // end displayType == DISPLAY_TYPE_QUAD
    flushHtml();

    // ── Gauge config ─────────────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_GAUGE || displayType == DISPLAY_TYPE_GAUGE_NUMBER) {
    for (int g = 0; g < 2; ++g) {
        if (g == 0) {
            html += "<div style='margin-bottom:8px;'><label>Show Bottom Gauge: <input type='checkbox' name='showbottom_" + String(s) + "'";
            if (screen_configs[s].show_bottom) html += " checked";
            html += "></label></div>";
        }
        int idx = s * 2 + g;
        if (g == 1 && !screen_configs[s].show_bottom) {
            html += "<div style='margin-bottom:8px;'><em>Bottom gauge disabled for this screen.</em></div>";
            continue;
        }
        html += "<b>" + String(g == 0 ? "Top Gauge" : "Bottom Gauge") + "</b>";
        flushHtml();
        html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field: ";
        n2k_select(html, "skpath_" + String(s) + "_" + String(g), signalk_paths[idx]);
        html += "</label></div>";
        // Calibration points
        html += "<table class='table'><tr><th>Point</th><th>Angle</th><th>Value</th><th>Test</th></tr>";
        for (int p = 0; p < 5; ++p) {
            html += "<tr><td>" + String(p+1) + "</td>";
            html += "<td><input name='angle_" + String(s) + "_" + String(g) + "_" + String(p) + "' type='number' value='" + String(gauge_cal[s][g][p].angle) + "'></td>";
            html += "<td><input name='value_" + String(s) + "_" + String(g) + "_" + String(p) + "' type='number' step='any' value='" + String(gauge_cal[s][g][p].value) + "'></td>";
            html += "<td><button type='button' onclick='testGaugePoint(" + String(s) + "," + String(g) + "," + String(p) + ")' ";
            html += (test_mode ? "" : "disabled ");
            html += "style='padding:4px 8px;font-size:0.9em;background-color:";
            html += (test_mode ? "#4a90e2" : "#cccccc");
            html += ";color:#fff;border:1px solid #2d5a8f;border-radius:4px;cursor:";
            html += (test_mode ? "pointer" : "not-allowed");
            html += ";'>Test</button></td></tr>";
        }
        html += "</table>";
        // Icon controls
        String savedIcon = String(screen_configs[s].icon_paths[g]);
        String savedIconNorm = savedIcon; savedIconNorm.toLowerCase();
        savedIconNorm.replace("S://", "S:/");
        while (savedIconNorm.indexOf("//") != -1) savedIconNorm.replace("//", "/");
        html += "<div class='icon-section'><div class='icon-row'>";
        html += "<div style='margin-bottom:8px;'><label>Icon: <select name='icon_" + String(s) + "_" + String(g) + "'>";
        html += "<option value=''";
        if (savedIcon.length() == 0) html += " selected='selected'";
        html += ">None</option>";
        for (const auto& icon : iconFiles) {
            String iconNorm = icon; iconNorm.toLowerCase();
            iconNorm.replace("S://", "S:/");
            while (iconNorm.indexOf("//") != -1) iconNorm.replace("//", "/");
            html += "<option value='" + icon + "'";
            if (iconNorm == savedIconNorm && savedIcon.length() > 0) html += " selected='selected'";
            html += ">" + icon + "</option>";
        }
        html += "</select></label></div>";
        // Icon position
        int curPos = screen_configs[s].icon_pos[g];
        html += "<div style='margin-bottom:8px;'><label>Icon Position: <select name='iconpos_" + String(s) + "_" + String(g) + "'>";
        struct { int v; const char *n; } posopts[] = { {0,"Top"}, {1,"Right"}, {2,"Bottom"}, {3,"Left"} };
        for (int _po = 0; _po < 4; ++_po) {
            html += "<option value='" + String(posopts[_po].v) + "'";
            if (curPos == posopts[_po].v) html += " selected='selected'";
            html += ">" + String(posopts[_po].n) + "</option>";
        }
        html += "</select></label></div></div>"; // close icon-row
        // Zone controls
        html += "<div class='zone-row'>";
        for (int i = 1; i <= 4; ++i) {
            float minVal = screen_configs[s].min[g][i];
            float maxVal = screen_configs[s].max[g][i];
            String colorVal = safeColor(screen_configs[s].color[g][i], "#000000");
            bool transVal = screen_configs[s].transparent[g][i] != 0;
            bool bzrVal = screen_configs[s].buzzer[g][i] != 0;
            html += "<div class='zone-item'><label>Min " + String(i) + ": <input name='mnv" + String(s) + String(g) + String(i) + "' type='number' step='any' value='" + String(minVal) + "' style='width:100px'></label></div>";
            html += "<div class='zone-item'><label>Max " + String(i) + ": <input name='mxv" + String(s) + String(g) + String(i) + "' type='number' step='any' value='" + String(maxVal) + "' style='width:100px'></label></div>";
            html += "<div class='zone-item'><label>Color: <input class='color-input' name='clr" + String(s) + String(g) + String(i) + "' type='color' value='" + colorVal + "'></label></div>";
            html += "<div class='zone-item small'><label>Transparent <input name='trn" + String(s) + String(g) + String(i) + "' type='checkbox'";
            if (transVal) html += " checked";
            html += "></label></div>";
            html += "<div class='zone-item small'><label>Buzzer <input name='bzr" + String(s) + String(g) + String(i) + "' type='checkbox'";
            if (bzrVal) html += " checked";
            html += "></label></div>";
        }
        html += "</div></div>"; // close zone-row + icon-section
        flushHtml();
    }
    } // end displayType == GAUGE or GAUGE_NUMBER
    flushHtml();

    // ── Gauge + Number config ────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_GAUGE_NUMBER) {
    html += "<h4>Center Number Display</h4>";
    flushHtml();
    html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "gauge_num_center_path_" + String(s), screen_configs[s].gauge_num_center_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:8px;'><label>Display Label (override): "
            "<input type='text' name='clabel_" + String(s) + "_0' placeholder='e.g. RPM' value='";
    html += g_custom_labels[s * 4 + 0];
    html += "' style='width:40%;'></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='gauge_num_center_font_size_" + String(s) + "'>";
    for (int fs = 0; fs < 5; fs++) {
        const char* fsNames[] = {"Small (48pt)","Medium (72pt)","Large (96pt)","X-Large (120pt)","XX-Large (144pt)"};
        html += "<option value='" + String(fs) + "'";
        if (screen_configs[s].gauge_num_center_font_size == fs) html += " selected";
        html += ">" + String(fsNames[fs]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='gauge_num_center_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].gauge_num_center_font_color[0] ? screen_configs[s].gauge_num_center_font_color : "#FFFFFF") + "'></label></div>";
    // Center number alarms
    html += "<div class='icon-section'><h5 style='margin:0 0 8px;'>Alarms</h5><div style='display:flex;gap:16px;flex-wrap:wrap;'>";
    html += "<div><label>Low Alarm &lt; <input name='gnum_low_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].min[1][1]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='gnum_low_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[1][1]) html += " checked";
    html += "> Enable</label></div>";
    html += "<div><label>High Alarm &gt; <input name='gnum_high_thresh_" + String(s) + "' type='number' step='any' value='" + String(screen_configs[s].max[1][2]) + "' style='width:90px'></label> ";
    html += "<label><input type='checkbox' name='gnum_high_buz_" + String(s) + "'";
    if (screen_configs[s].buzzer[1][2]) html += " checked";
    html += "> Enable</label></div></div></div>";
    } // end displayType == DISPLAY_TYPE_GAUGE_NUMBER
    flushHtml();

    // ── Graph config ─────────────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_GRAPH) {
    html += "<h4>Graph Display Settings</h4>";
    flushHtml();
    html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field: ";
    n2k_select(html, "graph_path_1_" + String(s), screen_configs[s].number_path);
    html += "</label></div>";
    html += "<div style='margin-bottom:8px;'><label>Chart Type: <select name='graph_chart_type_" + String(s) + "'>";
    const char* ctNames[] = {"Line Chart","Bar Chart","Scatter Plot"};
    for (int ct = 0; ct < 3; ct++) {
        html += "<option value='" + String(ct) + "'";
        if (screen_configs[s].graph_chart_type == ct) html += " selected";
        html += ">" + String(ctNames[ct]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Time Range: <select name='graph_time_range_" + String(s) + "'>";
    const char* trNames[] = {"10 seconds","30 seconds","1 minute","5 minutes","10 minutes","30 minutes"};
    for (int tr = 0; tr < 6; tr++) {
        html += "<option value='" + String(tr) + "'";
        if (screen_configs[s].graph_time_range == tr) html += " selected";
        html += ">" + String(trNames[tr]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Series 1 Color: <input name='graph_color_1_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_font_color[0] ? screen_configs[s].number_font_color : "#00FF00") + "'></label></div>";
    html += "<h5 style='margin-top:16px;'>Second Data Series (Optional)</h5>";
    flushHtml();
    html += "<div style='margin-bottom:8px;'><label>NMEA 2000 Field 2: ";
    n2k_select(html, "graph_path_2_" + String(s), screen_configs[s].graph_path_2);
    html += "</label></div>";
    html += "<div style='margin-bottom:8px;'><label>Series 2 Color: <input name='graph_color_2_" + String(s) + "' type='color' value='" + String(screen_configs[s].graph_color_2[0] ? screen_configs[s].graph_color_2 : "#FF0000") + "'></label></div>";
    bool isCustomColorGraph = (String(screen_configs[s].background_path) == "Custom Color");
    html += "<div id='graph_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColorGraph ? "block" : "none") + ";'>";
    html += "<label>Background Color: <input name='graph_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
    } // end displayType == DISPLAY_TYPE_GRAPH
    flushHtml();

    // ── Position Display config ──────────────────────────────────────
    if (displayType == DISPLAY_TYPE_POSITION) {
    html += "<h4>Position Display Settings</h4>";
    html += "<div style='margin-bottom:8px;'><label>Coordinate Format: <select name='pos_coord_format_" + String(s) + "'>";
    const char* cfNames[] = {"Decimal Degrees (DD)","Degrees Minutes Seconds (DMS)","Degrees Decimal Minutes (DDM)"};
    for (int cf = 0; cf < 3; cf++) {
        html += "<option value='" + String(cf) + "'";
        if (screen_configs[s].number_font_size == cf) html += " selected";
        html += ">" + String(cfNames[cf]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Lat/Lon Colour: <input name='pos_latlon_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].pos_latlon_color[0] ? screen_configs[s].pos_latlon_color : "#ffffff") + "'></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Time Colour: <input name='pos_time_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].pos_time_color[0] ? screen_configs[s].pos_time_color : "#64dcb4") + "'></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Divider / Title Colour: <input name='pos_divider_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].pos_divider_color[0] ? screen_configs[s].pos_divider_color : "#324678") + "'></label></div>";
    bool isCustomColorPos = (String(screen_configs[s].background_path) == "Custom Color");
    html += "<div id='pos_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColorPos ? "block" : "none") + ";'>";
    html += "<label>Background Colour: <input name='pos_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
    } // end displayType == DISPLAY_TYPE_POSITION
    flushHtml();

    // ── AIS Display config ───────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_AIS) {
    html += "<h4>AIS Radar Settings</h4>";
    html += "<div style='margin-bottom:8px;'><label>Range: <select name='ais_range_" + String(s) + "'>";
    const char* aisRangeNames[] = {"0.1 NM","0.5 NM","1 NM","2 NM","5 NM","10 NM","20 NM"};
    for (int ar = 0; ar < 7; ar++) {
        html += "<option value='" + String(ar) + "'";
        if (screen_configs[s].graph_time_range == ar) html += " selected";
        html += ">" + String(aisRangeNames[ar]) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'>";
    html += "<label>Background Colour: <input name='ais_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#001020") + "'></label></div>";
    } // end displayType == DISPLAY_TYPE_AIS
    flushHtml();

    // ── Attitude Display config ───────────────────────────────────────
    if (displayType == DISPLAY_TYPE_ATTITUDE) {
    html += "<h4>Attitude Indicator Settings</h4>";
    html += "<p style='color:#aaa;'>Uses NMEA 2000 PGN 127257 attitude data (heel/pitch sensor, autopilot, or MFD).</p>";
    html += "<p style='color:#aaa;'><b>Set Level</b> &mdash; snaps the current reading as the zero reference. <b>Clear Level</b> &mdash; removes the offset and uses raw N2K data.</p>";
    html += "<div style='display:flex;gap:12px;margin-bottom:8px;'>";
    html += "<button type='button' style='padding:8px 20px;font-size:16px;' onclick=\"fetch('/api/imu_calibrate',{method:'POST'}).then(r=>r.text()).then(t=>alert(t))\">Set Level</button>";
    html += "<button type='button' style='padding:8px 20px;font-size:16px;' onclick=\"fetch('/api/imu_clear',{method:'POST'}).then(r=>r.text()).then(t=>alert(t))\">Clear Level</button>";
    html += "</div>";
    } // end displayType == DISPLAY_TYPE_ATTITUDE

    // ── Anchor Alarm config ───────────────────────────────────────────────
    if (displayType == DISPLAY_TYPE_ANCHOR) {
    html += "<h4>Anchor Alarm Settings</h4>";
    html += "<p style='color:#aaa;'>Tap the map to place the anchor, or use <b>Drop Here</b> to drop at the current boat position. Use [&minus;]/[+] to adjust the alarm radius. Arm/disarm the alarm with the ALARM button.</p>";
    html += "<p style='color:#aaa;'>GPS position is required (NMEA 2000 PGN 129025/129029). Track history is stored in RAM and lost on reboot.</p>";
    // Depth source — stored in number_path (unused for anchor display type)
    {
        const char* cur = screen_configs[s].number_path;
        char buf50[50];
        snprintf(buf50, sizeof(buf50), "%d", (int)N2K_WATER_DEPTH);
        String wdVal  = String((int)N2K_WATER_DEPTH);
        String bkVal  = String((int)N2K_DEPTH_BELOW_KEEL);
        String offVal = String((int)N2K_DEPTH_OFFSET);
        html += "<div style='margin-bottom:8px;'><label>Depth display: ";
        html += "<select name='number_path_" + String(s) + "' style='width:200px'>";
        html += "<option value=''";
        if (!cur[0]) html += " selected";
        html += ">None</option>";
        html += "<option value='" + wdVal + "'";
        if (wdVal == String(cur)) html += " selected";
        html += ">Water Depth</option>";
        html += "<option value='" + bkVal + "'";
        if (bkVal == String(cur)) html += " selected";
        html += ">Depth Below Keel</option>";
        html += "<option value='" + offVal + "'";
        if (offVal == String(cur)) html += " selected";
        html += ">Depth Offset</option>";
        html += "</select></label></div>";
    }
    } // end displayType == DISPLAY_TYPE_ANCHOR
    flushHtml();

    // Send the entire fragment in one response with Content-Length.
    // lwIP handles flow control within its send window; iRAM usage is
    // bounded by TCP_SND_BUF (~5744 B) rather than accumulating across
    // every sendContent() call.
    // Connection: close prevents HTTP keep-alive pipelining, which can
    // confuse the synchronous single-client WebServer and cause truncated
    // responses (browser error: "Content-Length exceeds response body").
    esp_task_wdt_reset();
    config_server.sendHeader("Connection", "close");
    config_server.send(200, "text/html; charset=utf-8", html);
    html.clear();  // release PSRAM back to pool immediately
    // Do NOT rst_close_client() here — with Content-Length the browser
    // knows when the response ends and closes gracefully.  RST would
    // discard data still queued in the TCP send buffer for large responses.
    Serial.printf("[GAUGES] fragment s=%d complete, iRAM=%u\n", s,
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Display switching is handled exclusively by /gauges/switch (called by
    // showScreenTab on every tab click).  Do NOT call ui_set_screen() here —
    // the background preloader also hits this endpoint for all screens, which
    // would cycle the display through every screen and leave it on the last one.

    // Keep WS paused — the 60-second g_config_page_last_seen watchdog
    // (or navigating away) will resume it.  Do NOT resume here;
    // the browser may fetch the next tab seconds later and we'd
    // be thrashing TCP buffers with reconnect/disconnect cycles.
}

void handle_save_gauges() {
    if (config_server.method() == HTTP_POST) {
        bool reboot_needed = false;
        // AJAX save sends only one screen's fields + save_screen=N
        int save_only = -1;
        if (config_server.hasArg("save_screen")) {
            save_only = config_server.arg("save_screen").toInt();
            if (save_only < 0 || save_only >= NUM_SCREENS) save_only = -1;
        }
        Serial.printf("[SAVE] POST args=%d, save_screen=%d\n", config_server.args(), save_only);
        int s_start = (save_only >= 0) ? save_only : 0;
        int s_end   = (save_only >= 0) ? save_only + 1 : NUM_SCREENS;
        for (int s = s_start; s < s_end; ++s) {
            for (int g = 0; g < 2; ++g) {
                int idx = s * 2 + g;
                // Save SignalK path
                String skpathKey = "skpath_" + String(s) + "_" + String(g);
                if (config_server.hasArg(skpathKey)) {
                    signalk_paths[idx] = config_server.arg(skpathKey);
                }
                // Save icon selection
                String iconKey = "icon_" + String(s) + "_" + String(g);
                String iconValue = config_server.arg(iconKey);
                iconValue.replace("S://", "S:/");
                while (iconValue.indexOf("//") != -1) iconValue.replace("//", "/");
                // Icon changes are now handled by hot-apply, no reboot needed
                strncpy(screen_configs[s].icon_paths[g], iconValue.c_str(), 127);
                screen_configs[s].icon_paths[g][127] = '\0';
                // Save icon position (does not require reboot)
                String ipKey = "iconpos_" + String(s) + "_" + String(g);
                if (config_server.hasArg(ipKey)) {
                    int ipos = config_server.arg(ipKey).toInt();
                    if (ipos < 0) ipos = 0; if (ipos > 3) ipos = 3;
                    screen_configs[s].icon_pos[g] = (uint8_t)ipos;
                }
                // Save per-screen background (only once per screen - do this in the g==0 block)
                if (g == 0) {
                    String bgKey = "bg_" + String(s);
                    if (config_server.hasArg(bgKey)) {
                        String bgValue = config_server.arg(bgKey);
                        bgValue.replace("S://", "S:/");
                        while (bgValue.indexOf("//") != -1) bgValue.replace("//", "/");
                        if (strncmp(screen_configs[s].background_path, bgValue.c_str(), 127) != 0) {
                            reboot_needed = true;
                        }
                        strncpy(screen_configs[s].background_path, bgValue.c_str(), 127);
                        screen_configs[s].background_path[127] = '\0';
                    }
                }
                // Save show_bottom setting (only once per screen, handled in g==0 path so it's read here too)
                if (g == 0) {
                    String sbKey = "showbottom_" + String(s);
                    uint8_t new_sb = config_server.hasArg(sbKey) ? 1 : 0;
                    // Do not force reboot on show_bottom changes; handle via hot-apply below.
                    screen_configs[s].show_bottom = new_sb;
                    
                    // Save display_type setting
                    String dtKey = "displaytype_" + String(s);
                    if (config_server.hasArg(dtKey)) {
                        screen_configs[s].display_type = config_server.arg(dtKey).toInt();
                        Serial.printf("[SAVE] screen %d display_type=%d\n", s, screen_configs[s].display_type);
                    } else {
                        Serial.printf("[SAVE] screen %d displaytype_%d MISSING from POST\n", s, s);
                    }
                    
                    // Save number display settings
                    String numberPathKey = "number_path_" + String(s);
                    if (config_server.hasArg(numberPathKey)) {
                        strncpy(screen_configs[s].number_path, config_server.arg(numberPathKey).c_str(), 127);
                        screen_configs[s].number_path[127] = '\0';
                    }

                    // Save compass heading source (radio button → number_path)
                    String compassHdgKey = "compass_hdg_src_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_COMPASS && config_server.hasArg(compassHdgKey)) {
                        strncpy(screen_configs[s].number_path, config_server.arg(compassHdgKey).c_str(), 127);
                        screen_configs[s].number_path[127] = '\0';
                        Serial.printf("[SAVE] screen %d compass path=%s\n", s, screen_configs[s].number_path);
                    }
                    
                    // Note: background is now in bg_image field (can be bin file path or "Custom Color")
                    // Only save number bg color for NUMBER screens — other types use
                    // their own field names (dual_bg_color, quad_bg_color, graph_bg_color)
                    String numberBgColorKey = "number_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_NUMBER && config_server.hasArg(numberBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(numberBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }
                    
                    String numberFontSizeKey = "number_font_size_" + String(s);
                    if (config_server.hasArg(numberFontSizeKey)) {
                        screen_configs[s].number_font_size = config_server.arg(numberFontSizeKey).toInt();
                    }
                    
                    String numberFontColorKey = "number_font_color_" + String(s);
                    if (config_server.hasArg(numberFontColorKey)) {
                        strncpy(screen_configs[s].number_font_color, config_server.arg(numberFontColorKey).c_str(), 7);
                        screen_configs[s].number_font_color[7] = '\0';
                    }
                    
                    // Save number display alarm thresholds (reuse gauge zone slots 0/1 and 0/2)
                    // Only for NUMBER screens — dual/quad reuse the same slots
                    if (screen_configs[s].display_type == DISPLAY_TYPE_NUMBER) {
                        String numLowThreshKey = "num_low_thresh_" + String(s);
                        if (config_server.hasArg(numLowThreshKey)) {
                            screen_configs[s].min[0][1] = config_server.arg(numLowThreshKey).toFloat();
                        }
                        screen_configs[s].buzzer[0][1] = config_server.hasArg("num_low_buz_" + String(s)) ? 1 : 0;

                        String numHighThreshKey = "num_high_thresh_" + String(s);
                        if (config_server.hasArg(numHighThreshKey)) {
                            screen_configs[s].max[0][2] = config_server.arg(numHighThreshKey).toFloat();
                        }
                        screen_configs[s].buzzer[0][2] = config_server.hasArg("num_high_buz_" + String(s)) ? 1 : 0;
                    }

                    // Save dual display alarm thresholds — only for DUAL screens
                    // Top:    min[0][1]/buzzer[0][1]=low,  max[0][2]/buzzer[0][2]=high
                    // Bottom: min[1][1]/buzzer[1][1]=low,  max[1][2]/buzzer[1][2]=high
                    if (screen_configs[s].display_type == DISPLAY_TYPE_DUAL) {
                        if (config_server.hasArg("dual_top_low_thresh_" + String(s)))
                            screen_configs[s].min[0][1] = config_server.arg("dual_top_low_thresh_" + String(s)).toFloat();
                        screen_configs[s].buzzer[0][1] = config_server.hasArg("dual_top_low_buz_" + String(s)) ? 1 : 0;
                        if (config_server.hasArg("dual_top_high_thresh_" + String(s)))
                            screen_configs[s].max[0][2] = config_server.arg("dual_top_high_thresh_" + String(s)).toFloat();
                        screen_configs[s].buzzer[0][2] = config_server.hasArg("dual_top_high_buz_" + String(s)) ? 1 : 0;
                        if (config_server.hasArg("dual_bot_low_thresh_" + String(s)))
                            screen_configs[s].min[1][1] = config_server.arg("dual_bot_low_thresh_" + String(s)).toFloat();
                        screen_configs[s].buzzer[1][1] = config_server.hasArg("dual_bot_low_buz_" + String(s)) ? 1 : 0;
                        if (config_server.hasArg("dual_bot_high_thresh_" + String(s)))
                            screen_configs[s].max[1][2] = config_server.arg("dual_bot_high_thresh_" + String(s)).toFloat();
                        screen_configs[s].buzzer[1][2] = config_server.hasArg("dual_bot_high_buz_" + String(s)) ? 1 : 0;
                    }

                    // Save quad display alarm thresholds — only for QUAD screens
                    // TL=g0z1/2, TR=g0z3/4, BL=g1z1/2, BR=g1z3/4
                    if (screen_configs[s].display_type == DISPLAY_TYPE_QUAD) {
                        struct { const char* nm; int g, zl, zh; } qalms[4] = {
                            {"tl",0,1,2},{"tr",0,3,4},{"bl",1,1,2},{"br",1,3,4}
                        };
                        for (int q = 0; q < 4; q++) {
                            String lk = "quad_" + String(qalms[q].nm) + "_low_thresh_"  + String(s);
                            String hk = "quad_" + String(qalms[q].nm) + "_high_thresh_" + String(s);
                            if (config_server.hasArg(lk)) screen_configs[s].min[qalms[q].g][qalms[q].zl] = config_server.arg(lk).toFloat();
                            screen_configs[s].buzzer[qalms[q].g][qalms[q].zl] = config_server.hasArg("quad_" + String(qalms[q].nm) + "_low_buz_"  + String(s)) ? 1 : 0;
                            if (config_server.hasArg(hk)) screen_configs[s].max[qalms[q].g][qalms[q].zh] = config_server.arg(hk).toFloat();
                            screen_configs[s].buzzer[qalms[q].g][qalms[q].zh] = config_server.hasArg("quad_" + String(qalms[q].nm) + "_high_buz_" + String(s)) ? 1 : 0;
                        }
                    }

                    // Save dual display background color — only for DUAL screens
                    String dualBgColorKey = "dual_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_DUAL && config_server.hasArg(dualBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(dualBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }
                    
                    // Save dual display settings
                    String dualTopPathKey = "dual_top_path_" + String(s);
                    if (config_server.hasArg(dualTopPathKey)) {
                        strncpy(screen_configs[s].dual_top_path, config_server.arg(dualTopPathKey).c_str(), 127);
                        screen_configs[s].dual_top_path[127] = '\0';
                    }
                    
                    String dualTopFontSizeKey = "dual_top_font_size_" + String(s);
                    if (config_server.hasArg(dualTopFontSizeKey)) {
                        screen_configs[s].dual_top_font_size = config_server.arg(dualTopFontSizeKey).toInt();
                    }
                    
                    String dualTopFontColorKey = "dual_top_font_color_" + String(s);
                    if (config_server.hasArg(dualTopFontColorKey)) {
                        strncpy(screen_configs[s].dual_top_font_color, config_server.arg(dualTopFontColorKey).c_str(), 7);
                        screen_configs[s].dual_top_font_color[7] = '\0';
                    }
                    
                    String dualBottomPathKey = "dual_bottom_path_" + String(s);
                    if (config_server.hasArg(dualBottomPathKey)) {
                        strncpy(screen_configs[s].dual_bottom_path, config_server.arg(dualBottomPathKey).c_str(), 127);
                        screen_configs[s].dual_bottom_path[127] = '\0';
                        Serial.printf("[SAVE] dual_bottom_path_%d = '%s'\n", s, screen_configs[s].dual_bottom_path);
                    } else {
                        Serial.printf("[SAVE] dual_bottom_path_%d MISSING from POST\n", s);
                    }
                    
                    String dualBottomFontSizeKey = "dual_bottom_font_size_" + String(s);
                    if (config_server.hasArg(dualBottomFontSizeKey)) {
                        screen_configs[s].dual_bottom_font_size = config_server.arg(dualBottomFontSizeKey).toInt();
                    }
                    
                    String dualBottomFontColorKey = "dual_bottom_font_color_" + String(s);
                    if (config_server.hasArg(dualBottomFontColorKey)) {
                        strncpy(screen_configs[s].dual_bottom_font_color, config_server.arg(dualBottomFontColorKey).c_str(), 7);
                        screen_configs[s].dual_bottom_font_color[7] = '\0';
                    }
                    
                    // Save quad display background color — only for QUAD screens
                    String quadBgColorKey = "quad_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_QUAD && config_server.hasArg(quadBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(quadBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }

                    // Save compass background color — only for COMPASS screens
                    String compassBgColorKey = "compass_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_COMPASS && config_server.hasArg(compassBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(compassBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }

                    // Save compass extra data fields (BL/BR) — uses compass_bl_*/compass_br_* form names
                    if (screen_configs[s].display_type == DISPLAY_TYPE_COMPASS) {
                        auto saveCompassField = [&](const char* pos, char* path, uint8_t& size, char* color) {
                            String pk = "compass_" + String(pos) + "_path_" + String(s);
                            if (config_server.hasArg(pk)) { strncpy(path, config_server.arg(pk).c_str(), 127); path[127] = '\0'; }
                            String sk = "compass_" + String(pos) + "_font_size_" + String(s);
                            if (config_server.hasArg(sk)) { size = config_server.arg(sk).toInt(); }
                            String ck = "compass_" + String(pos) + "_font_color_" + String(s);
                            if (config_server.hasArg(ck)) { strncpy(color, config_server.arg(ck).c_str(), 7); color[7] = '\0'; }
                        };
                        saveCompassField("bl", screen_configs[s].quad_bl_path, screen_configs[s].quad_bl_font_size, screen_configs[s].quad_bl_font_color);
                        saveCompassField("br", screen_configs[s].quad_br_path, screen_configs[s].quad_br_font_size, screen_configs[s].quad_br_font_color);
                    }
                    
                    // Save quad display settings (TL, TR, BL, BR)
                    auto saveQuadrant = [&](const char* name, char* path, uint8_t& size, char* color) {
                        String pathKey = "quad_" + String(name) + "_path_" + String(s);
                        if (config_server.hasArg(pathKey)) {
                            strncpy(path, config_server.arg(pathKey).c_str(), 127);
                            path[127] = '\0';
                            Serial.printf("[SAVE] quad_%s_path_%d = '%s'\n", name, s, path);
                        } else {
                            Serial.printf("[SAVE] quad_%s_path_%d MISSING from POST\n", name, s);
                        }
                        String sizeKey = "quad_" + String(name) + "_font_size_" + String(s);
                        if (config_server.hasArg(sizeKey)) {
                            size = config_server.arg(sizeKey).toInt();
                        }
                        String colorKey = "quad_" + String(name) + "_font_color_" + String(s);
                        if (config_server.hasArg(colorKey)) {
                            strncpy(color, config_server.arg(colorKey).c_str(), 7);
                            color[7] = '\0';
                        }
                    };
                    
                    // Only apply quad form fields for actual Quad screens — the quad div
                    // is hidden for Compass screens but its inputs still get POSTed,
                    // which would overwrite the compass BL/BR settings.
                    if (screen_configs[s].display_type == DISPLAY_TYPE_QUAD) {
                        saveQuadrant("tl", screen_configs[s].quad_tl_path, screen_configs[s].quad_tl_font_size, screen_configs[s].quad_tl_font_color);
                        saveQuadrant("tr", screen_configs[s].quad_tr_path, screen_configs[s].quad_tr_font_size, screen_configs[s].quad_tr_font_color);
                        saveQuadrant("bl", screen_configs[s].quad_bl_path, screen_configs[s].quad_bl_font_size, screen_configs[s].quad_bl_font_color);
                        saveQuadrant("br", screen_configs[s].quad_br_path, screen_configs[s].quad_br_font_size, screen_configs[s].quad_br_font_color);
                    }
                    
                    // Save gauge+number display settings
                    String gaugeNumCenterPathKey = "gauge_num_center_path_" + String(s);
                    if (config_server.hasArg(gaugeNumCenterPathKey)) {
                        strncpy(screen_configs[s].gauge_num_center_path, config_server.arg(gaugeNumCenterPathKey).c_str(), 127);
                        screen_configs[s].gauge_num_center_path[127] = '\0';
                    }
                    
                    String gaugeNumCenterFontSizeKey = "gauge_num_center_font_size_" + String(s);
                    if (config_server.hasArg(gaugeNumCenterFontSizeKey)) {
                        screen_configs[s].gauge_num_center_font_size = config_server.arg(gaugeNumCenterFontSizeKey).toInt();
                    }
                    
                    String gaugeNumCenterFontColorKey = "gauge_num_center_font_color_" + String(s);
                    if (config_server.hasArg(gaugeNumCenterFontColorKey)) {
                        strncpy(screen_configs[s].gauge_num_center_font_color, config_server.arg(gaugeNumCenterFontColorKey).c_str(), 7);
                        screen_configs[s].gauge_num_center_font_color[7] = '\0';
                    }

                    // Save gauge+number center alarm thresholds — only for GAUGE_NUMBER screens
                    // min[1][1]/buzzer[1][1]=low, max[1][2]/buzzer[1][2]=high
                    if (screen_configs[s].display_type == DISPLAY_TYPE_GAUGE_NUMBER) {
                        if (config_server.hasArg("gnum_low_thresh_" + String(s)))
                            screen_configs[s].min[1][1] = config_server.arg("gnum_low_thresh_" + String(s)).toFloat();
                        screen_configs[s].buzzer[1][1] = config_server.hasArg("gnum_low_buz_" + String(s)) ? 1 : 0;
                        if (config_server.hasArg("gnum_high_thresh_" + String(s)))
                            screen_configs[s].max[1][2] = config_server.arg("gnum_high_thresh_" + String(s)).toFloat();
                        screen_configs[s].buzzer[1][2] = config_server.hasArg("gnum_high_buz_" + String(s)) ? 1 : 0;
                    }
                    
                    // Graph chart type
                    String graphChartTypeKey = "graph_chart_type_" + String(s);
                    if (config_server.hasArg(graphChartTypeKey)) {
                        screen_configs[s].graph_chart_type = config_server.arg(graphChartTypeKey).toInt();
                    }
                    
                    // Graph time range
                    String graphTimeRangeKey = "graph_time_range_" + String(s);
                    if (config_server.hasArg(graphTimeRangeKey)) {
                        screen_configs[s].graph_time_range = config_server.arg(graphTimeRangeKey).toInt();
                    }
                    
                    // Graph background color — only for GRAPH screens
                    String graphBgColorKey = "graph_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH && config_server.hasArg(graphBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(graphBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }

                    // Graph first series path — only apply for GRAPH screens to avoid
                    // overwriting NUMBER section's number_path on non-graph screens.
                    String graphPath1Key = "graph_path_1_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH && config_server.hasArg(graphPath1Key)) {
                        strncpy(screen_configs[s].number_path, config_server.arg(graphPath1Key).c_str(), 127);
                        screen_configs[s].number_path[127] = '\0';
                    }

                    // Graph first series colour — only apply for GRAPH screens to avoid
                    // overwriting NUMBER section's number_font_color on non-graph screens.
                    String graphColor1Key = "graph_color_1_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH && config_server.hasArg(graphColor1Key)) {
                        strncpy(screen_configs[s].number_font_color, config_server.arg(graphColor1Key).c_str(), 7);
                        screen_configs[s].number_font_color[7] = '\0';
                    }

                    // Graph second series path
                    String graphPath2Key = "graph_path_2_" + String(s);
                    if (config_server.hasArg(graphPath2Key)) {
                        strncpy(screen_configs[s].graph_path_2, config_server.arg(graphPath2Key).c_str(), 127);
                        screen_configs[s].graph_path_2[127] = '\0';
                    }
                    
                    // Graph second series color
                    String graphColor2Key = "graph_color_2_" + String(s);
                    if (config_server.hasArg(graphColor2Key)) {
                        strncpy(screen_configs[s].graph_color_2, config_server.arg(graphColor2Key).c_str(), 7);
                        screen_configs[s].graph_color_2[7] = '\0';
                    }

                    // Position coord format — only apply for POSITION screens to avoid
                    // overwriting NUMBER section's number_font_size on non-position screens.
                    String posCoordFmtKey = "pos_coord_format_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_POSITION && config_server.hasArg(posCoordFmtKey)) {
                        screen_configs[s].number_font_size = (uint8_t)config_server.arg(posCoordFmtKey).toInt();
                    }

                    // Position display colours
                    String posLatlonColorKey = "pos_latlon_color_" + String(s);
                    if (config_server.hasArg(posLatlonColorKey)) {
                        strncpy(screen_configs[s].pos_latlon_color, config_server.arg(posLatlonColorKey).c_str(), 7);
                        screen_configs[s].pos_latlon_color[7] = '\0';
                    }
                    String posTimeColorKey = "pos_time_color_" + String(s);
                    if (config_server.hasArg(posTimeColorKey)) {
                        strncpy(screen_configs[s].pos_time_color, config_server.arg(posTimeColorKey).c_str(), 7);
                        screen_configs[s].pos_time_color[7] = '\0';
                    }
                    String posDividerColorKey = "pos_divider_color_" + String(s);
                    if (config_server.hasArg(posDividerColorKey)) {
                        strncpy(screen_configs[s].pos_divider_color, config_server.arg(posDividerColorKey).c_str(), 7);
                        screen_configs[s].pos_divider_color[7] = '\0';
                    }
                    // Position background colour — only apply for POSITION screens to avoid
                    // overwriting NUMBER section's number_bg_color on non-position screens.
                    String posBgColorKey = "pos_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_POSITION && config_server.hasArg(posBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(posBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }

                    // AIS range — only apply for AIS screens to avoid overwriting
                    // graph_time_range on non-AIS screens.
                    String aisRangeKey = "ais_range_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_AIS && config_server.hasArg(aisRangeKey)) {
                        screen_configs[s].graph_time_range = (uint8_t)config_server.arg(aisRangeKey).toInt();
                    }

                    // AIS background colour — only apply for AIS screens
                    String aisBgColorKey = "ais_bg_color_" + String(s);
                    if (screen_configs[s].display_type == DISPLAY_TYPE_AIS && config_server.hasArg(aisBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(aisBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }
                }
                // Only process zone settings if the first zone field is in the form
                // (gauges hidden by display type won't submit their zone fields)
                String firstZoneKey = "mnv" + String(s) + String(g) + "1";
                // Only save gauge zones for actual gauge screens.
                // NUMBER, DUAL, QUAD screens reuse these slots for alarm thresholds;
                // saving the (hidden) gauge form fields would overwrite those values.
                if (config_server.hasArg(firstZoneKey) &&
                    (screen_configs[s].display_type == DISPLAY_TYPE_GAUGE ||
                     screen_configs[s].display_type == DISPLAY_TYPE_GAUGE_NUMBER)) {
                    for (int i = 1; i <= 4; ++i) {
                        String minKey = "mnv" + String(s) + String(g) + String(i);
                        String maxKey = "mxv" + String(s) + String(g) + String(i);
                        String colorKey = "clr" + String(s) + String(g) + String(i);
                        String transKey = "trn" + String(s) + String(g) + String(i);
                        screen_configs[s].min[g][i] = config_server.arg(minKey).toFloat();
                        screen_configs[s].max[g][i] = config_server.arg(maxKey).toFloat();
                        strncpy(screen_configs[s].color[g][i], config_server.arg(colorKey).c_str(), 7);
                        screen_configs[s].color[g][i][7] = '\0';
                        screen_configs[s].transparent[g][i] = config_server.hasArg(transKey) ? 1 : 0;
                        String buzKey = "bzr" + String(s) + String(g) + String(i);
                        screen_configs[s].buzzer[g][i] = config_server.hasArg(buzKey) ? 1 : 0;
                    }
                }
                // Only process calibration data if the angle fields are actually in the form
                // (gauges hidden by display type won't submit their fields)
                String firstAngleKey = "angle_" + String(s) + "_" + String(g) + "_0";
                if (config_server.hasArg(firstAngleKey)) {
                    for (int p = 0; p < 5; ++p) {
                        String angleKey = "angle_" + String(s) + "_" + String(g) + "_" + String(p);
                        String valueKey = "value_" + String(s) + "_" + String(g) + "_" + String(p);
                        gauge_cal[s][g][p].angle = config_server.arg(angleKey).toInt();
                        gauge_cal[s][g][p].value = config_server.arg(valueKey).toFloat();
                    }
                }
            }
        }

        // Sync gauge_cal → screen_configs.cal before SD/NVS writes.
        // The form values were written to gauge_cal above, but screen_configs.cal
        // still holds the old values. Without this copy the calibration angles
        // are silently lost when saving to SD.
        for (int s = 0; s < NUM_SCREENS; ++s)
            for (int g = 0; g < 2; ++g)
                for (int p = 0; p < 5; ++p)
                    screen_configs[s].cal[g][p] = gauge_cal[s][g][p];

        // Debug: dump quad/dual bottom paths before SD write
        for (int s = s_start; s < s_end; ++s) {
            Serial.printf("[PRE-SD] s=%d quad: tl='%s' tr='%s' bl='%s' br='%s'\n", s,
                screen_configs[s].quad_tl_path, screen_configs[s].quad_tr_path,
                screen_configs[s].quad_bl_path, screen_configs[s].quad_br_path);
            Serial.printf("[PRE-SD] s=%d dual: top='%s' bottom='%s'\n", s,
                screen_configs[s].dual_top_path, screen_configs[s].dual_bottom_path);
        }

        // Attempt to write per-screen binary configs to SD immediately so toggles
        // (like show_bottom) persist even if NVS writes fail or are delayed.
        //
        // iRAM strategy: pause_signalk_ws() disconnects the WS (freeing ~22 KB WS
        // receive buffer) before every SD write block. This is called here in the
        // save handler — not just in handle_gauges_page() — because the WS reconnects
        // seconds after each save so subsequent saves arrive with iRAM already low.
        // Pausing here guarantees ~22 KB headroom for SDMMC DMA on every save.
        // A short yield after the pause lets lwIP free any remaining TCP buffers.
        pause_signalk_ws();
        pause_n2k_updates();
        {
            const size_t IRAM_MIN_FOR_SD = 20 * 1024;
            size_t iram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (iram_free < IRAM_MIN_FOR_SD) {
                Serial.printf("[SD SAVE] iRAM still low after WS pause (%u B), yielding...\n", iram_free);
                Serial.flush();
                for (int w = 0; w < 20; w++) {  // up to 1s
                    vTaskDelay(pdMS_TO_TICKS(50));
                    esp_task_wdt_reset();
                    iram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                    if (iram_free >= IRAM_MIN_FOR_SD) break;
                }
                Serial.printf("[SD SAVE] iRAM after yield: %u B\n", iram_free);
                Serial.flush();
            }
        }
        // Verify SD is still mounted; attempt remount if needed.
        // Once a remount fails, skip all future attempts this boot to avoid
        // blocking the save handler with multi-hundred-ms SD init timeouts.
        static bool sd_permanently_absent = false;
        bool sd_available = true;
        if (sd_permanently_absent) {
            sd_available = false;
        } else if (SD_MMC.cardType() == CARD_NONE) {
            Serial.println("[SD SAVE] Card not mounted, attempting remount...");
            SD_MMC.end();
            if (!SD_MMC.begin("/sdcard", true)) {
                Serial.println("[SD SAVE] Remount failed, falling back to NVS");
                sd_permanently_absent = true;
                sd_available = false;
            }
        }
        int sd_ok_count = 0;
        bool sd_all_ok = false;
        if (sd_available) {
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        // Batch write: all screens in one file — 3 FAT ops instead of 15
        {
            size_t total = sizeof(ScreenConfig) * NUM_SCREENS;
            File sf;
            for (int retry = 0; retry < 3 && !sf; retry++) {
                if (retry > 0) { vTaskDelay(pdMS_TO_TICKS(50)); esp_task_wdt_reset(); }
                sf = SD_MMC.open("/config/screens.bin.tmp", FILE_WRITE);
            }
            if (sf) {
                size_t wrote = sf.write((const uint8_t *)screen_configs, total);
                sf.flush();
                sf.close();
                if (wrote == total) {
                    SD_MMC.remove("/config/screens.bin");  // remove old only after full write
                    SD_MMC.rename("/config/screens.bin.tmp", "/config/screens.bin");  // atomic replace
                    Serial.printf("[SD SAVE] Wrote /config/screens.bin -> %u bytes\n", (unsigned)wrote);
                    sd_ok_count = NUM_SCREENS;
                } else {
                    SD_MMC.remove("/config/screens.bin.tmp");  // discard partial; original intact
                    Serial.printf("[SD SAVE] Short write /config/screens.bin -> %u/%u B, original preserved\n",
                                  (unsigned)wrote, (unsigned)total);
                }
            } else {
                Serial.println("[SD SAVE] Failed to open /config/screens.bin.tmp for writing");
            }
        }
        sd_all_ok = (sd_ok_count == NUM_SCREENS);
        if (sd_all_ok) Serial.printf("[SD SAVE] All %d screens OK, skipping NVS blob writes\n", NUM_SCREENS);

        // Save SignalK gauge paths to SD so they persist without NVS writes.
        // save_preferences() writes 21 NVS keys (ssid, pw, skpaths×10, etc.) causing
        // NVS page-cache iRAM growth (~400 bytes per save). Instead write skpaths to
        // an SD text file (one path per line) and skip save_preferences() entirely
        // when SD is healthy. WiFi/device settings don't change on this page.
        if (sd_all_ok) {
            File spf;
            for (int retry = 0; retry < 3 && !spf; retry++) {
                if (retry > 0) { vTaskDelay(pdMS_TO_TICKS(50)); esp_task_wdt_reset(); }
                spf = SD_MMC.open("/config/signalk_paths.tmp", FILE_WRITE);
            }
            if (spf) {
                for (int i = 0; i < NUM_SCREENS * 2; ++i) {
                    spf.println(signalk_paths[i]);
                }
                spf.flush();
                spf.close();
                SD_MMC.remove("/config/signalk_paths.txt");
                SD_MMC.rename("/config/signalk_paths.tmp", "/config/signalk_paths.txt");
                Serial.println("[SD SAVE] Wrote /config/signalk_paths.txt");
            } else {
                Serial.println("[SD SAVE] Failed to write /config/signalk_paths.txt — falling back to NVS");
                sd_all_ok = false;  // trigger NVS save below
            }
        }
        // Parse and save custom display labels (one text field per slot per screen)
        for (int s = s_start; s < s_end; ++s) {
            for (int slot = 0; slot < 4; ++slot) {
                String key = "clabel_" + String(s) + "_" + String(slot);
                if (config_server.hasArg(key)) {
                    g_custom_labels[s * 4 + slot] = config_server.arg(key);
                }
            }
        }
        if (sd_all_ok) {
            File lf = SD_MMC.open("/config/screen_labels.tmp", FILE_WRITE);
            if (lf) {
                for (int i = 0; i < NUM_SCREENS * 4; ++i) lf.println(g_custom_labels[i]);
                lf.flush(); lf.close();
                SD_MMC.remove("/config/screen_labels.txt");
                SD_MMC.rename("/config/screen_labels.tmp", "/config/screen_labels.txt");
                Serial.println("[SD SAVE] Wrote /config/screen_labels.txt");
            }
        }
        } // end if (sd_available)
        if (!sd_all_ok) {
            // SD failed: fall back to full NVS persist
            save_preferences(false);
        } else {
            // SD succeeded: persist WiFi/device settings to NVS, skip screen blobs
            save_preferences(true);
        }

        // Do NOT schedule WS resume here — the user is still on the config page
        // and will immediately re-pause WS when clicking another tab.  Resuming
        // creates a connect→disconnect cycle whose TIME_WAIT PCBs consume hidden
        // iRAM and crash the next fragment.  The 10-second idle watchdog in loop()
        // resumes WS automatically once the user stops accessing the config page.
        // Note: fetch_all_metadata() intentionally NOT called here — it makes blocking
        // HTTP requests (up to 1.5s each × many paths) which causes WDT on Core 1.
        // Metadata is fetched automatically on WS connect (wsEvent WStype_CONNECTED).

        // Defer LVGL rebuild to loop() — never call apply_all_screen_visuals() from
        // inside handleClient(). Doing so modifies LVGL objects (lv_obj_del/create)
        // from the HTTP handler, which races with the display DMA flush and corrupts
        // the heap after repeated page-builds, causing LoadProhibited crashes.
        g_pending_visual_apply = true;
        // Only mark the screen(s) that were actually saved — rebuilding all 5
        // wastes iRAM on SD background-image DMA reads for unchanged screens.
        if (save_only >= 0) {
            g_screens_need_apply[save_only] = true;
        } else {
            for (int i = 0; i < 5; i++) g_screens_need_apply[i] = true;
        }
        skip_next_load_preferences = true;

        // Keep the config-page idle watchdog alive so the 60-second auto-resume
        // doesn't fire while the user is still editing after a save.
        g_config_page_last_seen = millis();

        // Return tiny JSON — browser stays on the same page (AJAX save).
        config_server.sendHeader("Connection", "close");
        config_server.send(200, "application/json", "{\"ok\":true}");
        return;
    } else {
        config_server.send(405, "text/plain", "Method Not Allowed");
    }
}

void handle_root() {
    int cs = ui_get_current_screen();
    String html = "<html><head>";
    html += STYLE;
    html += "<title>ESP32 Gauge Config</title></head><body><div class='container'>";
    html += "<div class='tab-content' style='text-align:center;'>";
    html += "<h1>ESP32 Gauge Config</h1>";
    html += "<div class='status'>Status: " + String(WiFi.isConnected() ? "Connected" : "AP Mode") + "<br>IP: " + WiFi.localIP().toString();
    if (saved_hostname.length()) {
        html += "<br>Hostname: " + (saved_hostname + ".local") + "</div>";
    } else {
        html += "<br>Hostname: (not set)</div>";
    }
    // Screens selector in a colored container
    html += "<div class='screens-container'>";
    html += "<div class='screens-title'>Screens</div>";
    html += "<div class='screens-row'>";
    for (int i = 1; i <= NUM_SCREENS; ++i) {
        String redirect = "/set-screen?screen=" + String(i);
        if (i == cs) {
            html += "<button class='tab-btn' style='background:#d0e9ff;font-weight:700' onclick=\"location.href='" + redirect + "'\">Screen " + String(i) + "</button>";
        } else {
            html += "<button class='tab-btn' onclick=\"location.href='" + redirect + "'\">Screen " + String(i) + "</button>";
        }
    }
    html += "</div></div>";

    html += "<div class='root-actions' style='margin-top:12px;'>";
    html += "<button class='tab-btn' onclick=\"location.href='/network'\">Network Setup</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/gauges'\">Gauge Calibration</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/needles'\">Needles</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/assets'\">Assets</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/device'\">Device Settings</button>";
    html += "<button class='tab-btn' onclick=\"location.href='/update'\">Firmware Update</button>";
    html += "</div>"; // root-actions
    html += "<div style='text-align:center;margin-top:18px;font-size:0.8em;color:#888;'>Firmware: " + String(FW_VERSION) + "</div>";
    html += "</div>"; // tab-content
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

// ── Background WiFi scan cache ───────────────────────────────────────────────
// The scan runs asynchronously from poll_wifi_scan() called in loop().
// handle_scan_wifi() returns the cached result instantly so the HTTP
// connection is never held open during the radio sweep (which drops AP clients).
static String   g_wifi_scan_cache    = "[]";  // last completed scan JSON
static bool     g_wifi_scan_running  = false;
static uint32_t g_wifi_scan_started  = 0;

// Called from loop() — starts and collects async scans outside any HTTP handler.
void poll_wifi_scan() {
    if (!g_wifi_scan_running) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        // Still running — time out after 8 s to avoid a stuck state
        if (millis() - g_wifi_scan_started > 8000) {
            WiFi.scanDelete();
            g_wifi_scan_running = false;
        }
        return;
    }
    // Scan finished (n >= 0) or failed (n == WIFI_SCAN_FAILED)
    if (n < 0) n = 0;
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
    g_wifi_scan_cache   = json;
    g_wifi_scan_running = false;
}

void handle_scan_wifi() {
    // Kick off a new background scan (no-op if one is already running).
    if (!g_wifi_scan_running) {
        WiFi.scanNetworks(/*async=*/true);
        g_wifi_scan_running = true;
        g_wifi_scan_started = millis();
        // Return "scanning" so the browser retries — do NOT block here.
        config_server.send(202, "application/json", "{\"scanning\":true}");
        return;
    }
    // A scan is in progress — tell the browser to retry in a moment.
    config_server.send(202, "application/json", "{\"scanning\":true}");
}

void handle_scan_wifi_results() {
    // Returns the cached results (or empty array if no scan done yet).
    config_server.send(200, "application/json", g_wifi_scan_cache);
}

static String rssi_bar(int rssi) {
    // Returns a simple text/HTML indicator for signal strength
    int pct = constrain(2 * (rssi + 100), 0, 100);
    String color = (pct > 60) ? "#2a2" : (pct > 30) ? "#da2" : "#d22";
    return String(rssi) + " dBm (" + String(pct) + "%) "
         + "<span style='display:inline-block;width:80px;background:#eee;border-radius:3px;height:12px;vertical-align:middle'>"
         + "<span style='display:inline-block;width:" + String(pct) + "%;background:" + color + ";height:100%;border-radius:3px'></span></span>";
}

void handle_network_page() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>Network Setup</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Network Setup</h2>";

    // WiFi status / signal strength panel
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
    // SSID row with scan button
    html += "<div class='form-row'><label>SSID:</label>"
            "<input id='ssid' name='ssid' type='text' value='" + saved_ssid + "' style='width:40%'>"
            " <button type='button' id='scanBtn' onclick='scanWifi()' style='padding:6px 12px;cursor:pointer'>Scan</button>"
            "</div>";
    html += "<div id='scanResults' style='margin:0 0 10px 148px;display:none'></div>";
    html += "<div class='form-row'><label>Password:</label><input name='password' type='password' value='" + saved_password + "'></div>";

    html += "<div class='form-row'><label>NMEA 2000:</label><span style='color:#4a4;font-weight:bold'>CAN Bus (TX=" + String(CAN_TX_PIN) + ", RX=" + String(CAN_RX_PIN) + ")</span></div>";

    html += "<div class='form-row'><label>ESP32 Hostname:</label><input name='hostname' type='text' value='" + saved_hostname + "'></div>";
    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save & Reboot</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";

    // JavaScript for WiFi scanning — two-phase: kick /scan-wifi then poll /scan-wifi-results
    html += "<script>"
            "function renderNets(nets){"
              "var btn=document.getElementById('scanBtn');"
              "var div=document.getElementById('scanResults');"
              "if(!nets.length){div.innerHTML='No networks found.';btn.disabled=false;btn.textContent='Scan';return;}"
              "nets.sort(function(a,b){return b.rssi-a.rssi;});"
              "var seen={};"
              "var t='<table style=\"width:100%;border-collapse:collapse;font-size:0.9em\">';"
              "t+='<tr style=\"background:#e3edf7\"><th style=\"padding:4px 6px;text-align:left\">SSID</th><th>Signal</th><th>Sec</th><th></th></tr>';"
              "nets.forEach(function(n){"
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
            "}"
            "function pollResults(attempts){"
              "if(attempts<=0){document.getElementById('scanResults').innerHTML='Scan timed out.';document.getElementById('scanBtn').disabled=false;document.getElementById('scanBtn').textContent='Scan';return;}"
              "setTimeout(function(){"
                "fetch('/scan-wifi-results').then(function(r){return r.json();}).then(function(nets){"
                  "renderNets(nets);"
                "}).catch(function(){pollResults(attempts-1);});"
              "},1500);"
            "}"
            "function scanWifi(){"
              "var btn=document.getElementById('scanBtn');"
              "var div=document.getElementById('scanResults');"
              "btn.disabled=true;btn.textContent='Scanning...';"
              "div.style.display='block';div.innerHTML='Scanning (takes ~4s)...';"
              // Kick off the scan — returns 202 immediately, radio sweeps in background
              "fetch('/scan-wifi').then(function(){pollResults(8);}).catch(function(e){div.innerHTML='Scan error: '+e;btn.disabled=false;btn.textContent='Scan';});"
            "}"
            "function pickSsid(s){document.getElementById('ssid').value=s;}"
            "</script>";

    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}


void handle_save_wifi() {
    if (config_server.method() == HTTP_POST) {
        saved_ssid = config_server.arg("ssid");
        saved_password = config_server.arg("password");
        saved_signalk_ip = config_server.arg("signalk_ip");
        saved_signalk_port = config_server.arg("signalk_port").toInt();
        saved_hostname = config_server.arg("hostname");
        save_preferences();
        Serial.println("[WiFi Config] SSID: " + saved_ssid);
        Serial.println("[WiFi Config] SignalK IP: " + saved_signalk_ip);
        Serial.printf("[WiFi Config] SignalK Port: %u\n", saved_signalk_port);
        Serial.println("[WiFi Config] Hostname: " + saved_hostname);
        String html = "<html><head>";
        html += STYLE;
        html += "<title>Saved</title></head><body><div class='container'>";
        html += "<h2>Settings saved.<br>Rebooting...</h2>";
        html += "</div></body></html>";
        config_server.send(200, "text/html", html);
        delay(1000);
        ESP.restart();
    } else {
        config_server.send(405, "text/plain", "Method Not Allowed");
    }
}

void handle_device_page() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>Device Settings</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Device Settings</h2>";
    html += "<form method='POST' action='/save-device'>";
    // Buzzer mode + cooldown on one row
    html += "<div class='form-row'><label>Buzzer:</label><select name='buzzer_mode'>";
    html += "<option value='0'" + String(buzzer_mode==0?" selected":"") + ">Off</option>";
    html += "<option value='1'" + String(buzzer_mode==1?" selected":"") + ">Global</option>";
    html += "<option value='2'" + String(buzzer_mode==2?" selected":"") + ">Per-screen</option>";
    html += "</select><select name='buzzer_cooldown'>";
    html += "<option value='0'" + String(buzzer_cooldown_sec==0?" selected":"") + ">Constant</option>";
    html += "<option value='5'" + String(buzzer_cooldown_sec==5?" selected":"") + ">5s pause</option>";
    html += "<option value='10'" + String(buzzer_cooldown_sec==10?" selected":"") + ">10s pause</option>";
    html += "<option value='30'" + String(buzzer_cooldown_sec==30?" selected":"") + ">30s pause</option>";
    html += "<option value='60'" + String(buzzer_cooldown_sec==60?" selected":"") + ">60s pause</option>";
    html += "</select></div>";
    // Auto-scroll (dropdown matching screen options)
    html += "<div class='form-row'><label>Auto-scroll:</label><select name='auto_scroll'>";
    html += "<option value='0'" + String(auto_scroll_sec==0?" selected":"") + ">Off</option>";
    html += "<option value='5'" + String(auto_scroll_sec==5?" selected":"") + ">5s</option>";
    html += "<option value='10'" + String(auto_scroll_sec==10?" selected":"") + ">10s</option>";
    html += "<option value='30'" + String(auto_scroll_sec==30?" selected":"") + ">30s</option>";
    html += "<option value='60'" + String(auto_scroll_sec==60?" selected":"") + ">60s</option>";
    html += "</select></div>";
    // Screen off timeout
    html += "<div class='form-row'><label>Screen Sleep:</label><select name='screen_off_timeout'>";
    html += "<option value='0'"  + String(screen_off_timeout_min==0 ?" selected":"") + ">Always on</option>";
    html += "<option value='1'"  + String(screen_off_timeout_min==1 ?" selected":"") + ">1 min</option>";
    html += "<option value='5'"  + String(screen_off_timeout_min==5 ?" selected":"") + ">5 min</option>";
    html += "<option value='10'" + String(screen_off_timeout_min==10?" selected":"") + ">10 min</option>";
    html += "<option value='30'" + String(screen_off_timeout_min==30?" selected":"") + ">30 min</option>";
    html += "</select></div>";
    // Brightness — V4: unified slider (10-200, with red mode below 110), V3: 4-step slider
    if (is_board_v4()) {
        html += "<div class='form-row'><label>Brightness:</label>";
        html += "<input type='range' name='brightness_lv' min='10' max='200' value='" + String(brightness_level) + "'";
        html += " oninput='var v=parseInt(this.value),s=this.nextElementSibling;";
        html += "if(v>=110){var f=(v-110)/90;var p=Math.round(10+f*f*90);s.textContent=p+\"%\";s.style.color=\"#111\";}";
        html += "else{var f=(v-10)/99;var p=Math.round(10+f*f*90);s.textContent=\"R:\"+p+\"%\";s.style.color=\"#f44\";}' style='width:120px;vertical-align:middle;'>";
        {
            float frac;
            int pwm;
            if (brightness_level >= 110) {
                frac = (float)(brightness_level - 110) / 90.0f;
            } else {
                frac = (float)(brightness_level - 10) / 99.0f;
            }
            pwm = 10 + (int)(frac * frac * 90.0f);
            if (brightness_level >= 110) {
                html += "<span style='margin-left:8px;color:#111;'>" + String(pwm) + "%</span>";
            } else {
                html += "<span style='margin-left:8px;color:#f44;'>R:" + String(pwm) + "%</span>";
            }
        }
        html += "</div>";
    } else {
        static const char *blabels[] = {"Normal", "Dim", "Night", "Night+"};
        html += "<div class='form-row'><label>Brightness:</label>";
        html += "<input type='range' name='brightness_lv' min='0' max='3' value='" + String(brightness_level) + "'";
        html += " oninput='this.nextElementSibling.textContent=[\"Normal\",\"Dim\",\"Night\",\"Night+\"][this.value]'";
        html += " style='width:120px;vertical-align:middle;'>";
        html += "<span style='margin-left:8px;color:#fff;'>" + String(blabels[brightness_level < 4 ? brightness_level : 0]) + "</span>";
        html += "</div>";
    }
    // Unit system
    html += "<div class='form-row'><label>Units:</label><select name='unit_system'>";
    html += "<option value='0'" + String(unit_system==UNIT_METRIC?" selected":"") + ">Metric</option>";
    html += "<option value='1'" + String(unit_system==UNIT_IMPERIAL_US?" selected":"") + ">Imperial US</option>";
    html += "<option value='2'" + String(unit_system==UNIT_IMPERIAL_UK?" selected":"") + ">Imperial UK</option>";
    html += "<option value='3'" + String(unit_system==UNIT_NAUTICAL_METRIC?" selected":"") + ">Nautical Metric</option>";
    html += "<option value='4'" + String(unit_system==UNIT_NAUTICAL_IMP_US?" selected":"") + ">Nautical Imperial US</option>";
    html += "<option value='5'" + String(unit_system==UNIT_NAUTICAL_IMP_UK?" selected":"") + ">Nautical Imperial UK</option>";
    html += "</select></div>";
    // Own MMSI for AIS self-filtering
    html += "<div class='form-row'><label>Own MMSI:</label><input name='own_mmsi' type='text' maxlength='9' placeholder='e.g. 123456789' value='" + saved_own_mmsi + "'></div>";
    html += "<div id='unit-summary' style='margin:10px 0;padding:8px 12px;background:#1a1a2e;border-radius:6px;font-size:13px;color:#ccc;line-height:1.6;'></div>";
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
    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_save_device() {
    if (config_server.method() == HTTP_POST) {
        // Read and apply posted values
        int bm = config_server.arg("buzzer_mode").toInt();
        uint16_t bcd = (uint16_t)config_server.arg("buzzer_cooldown").toInt();
        uint16_t asc = (uint16_t)config_server.arg("auto_scroll").toInt();

        buzzer_mode = bm;
        buzzer_cooldown_sec = bcd;
        // Arm cooldown so first alarm fires after one cooldown period, not immediately.
        // Prevents spurious alarm on 0.00 value when mode is first enabled.
        first_run_buzzer = false;
        last_buzzer_time = millis();
        Serial.printf("[DEVICE SAVE_POST] buzzer_mode=%d buzzer_cooldown_sec=%u first_run_buzzer=%d auto_scroll=%u\n",
                  buzzer_mode, buzzer_cooldown_sec, (int)first_run_buzzer, (unsigned)auto_scroll_sec);

        auto_scroll_sec = asc;
        // Apply auto-scroll at runtime
        set_auto_scroll_interval(auto_scroll_sec);

        // Screen off timeout
        uint16_t sot = (uint16_t)config_server.arg("screen_off_timeout").toInt();
        // Only accept the allowed values; anything else → always on
        if (sot != 1 && sot != 5 && sot != 10 && sot != 30) sot = 0;
        screen_off_timeout_min = sot;

        // Brightness level
        uint8_t bl = (uint8_t)config_server.arg("brightness_lv").toInt();
        if (is_board_v4()) {
            int bl_raw = config_server.arg("brightness_lv").toInt();
            if (bl_raw > 200) bl_raw = 200;
            if (bl_raw < 10) bl_raw = 10;
            bl = (uint8_t)bl_raw;
        } else {
            if (bl > 3) bl = 0;
        }
        set_brightness_level(bl);

        // Unit system
        uint16_t us = (uint16_t)config_server.arg("unit_system").toInt();
        if (us > 5) us = 3; // default to nautical metric
        unit_system = (UnitSystem)us;

        // Own MMSI
        saved_own_mmsi = config_server.arg("own_mmsi");

        // Persist settings
        save_preferences();

        // Redirect back to device page
        config_server.sendHeader("Location", "/device", true);
        config_server.send(302, "text/plain", "");
        return;
    }
    config_server.send(405, "text/plain", "Method Not Allowed");
}

// Needle style WebUI handlers
void handle_needles_page() {
    if (config_server.method() != HTTP_GET) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    int screen = 0;
    int gauge = 0;
    if (config_server.hasArg("screen")) screen = config_server.arg("screen").toInt();
    if (config_server.hasArg("gauge")) gauge = config_server.arg("gauge").toInt();
    if (screen < 0) screen = 0; if (screen >= NUM_SCREENS) screen = NUM_SCREENS - 1;
    if (gauge < 0) gauge = 0; if (gauge > 1) gauge = 0;
    NeedleStyle s = get_needle_style(screen, gauge);

    String html = "<html><head>";
    html += STYLE;
    html += "<title>Needle Styles</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Needle Styles</h2>";
    html += "<form method='POST' action='/save-needles'>";
    // Screen/gauge selectors
    html += "<div class='form-row'><label>Screen:</label><select name='screen'>";
    for (int i = 0; i < NUM_SCREENS; ++i) {
        // keep option value 0-based for backend, show 1-based to user
        html += "<option value='" + String(i) + "'" + String(i==screen?" selected":"") + ">" + String(i+1) + "</option>";
    }
    html += "</select></div>";
    html += "<div class='form-row'><label>Gauge:</label><select name='gauge'>";
    html += "<option value='0'" + String(gauge==0?" selected":"") + ">Top</option>";
    html += "<option value='1'" + String(gauge==1?" selected":"") + ">Bottom</option>";
    html += "</select></div>";
    // Color
    html += "<div class='form-row'><label>Color:</label><input name='color' type='color' value='" + s.color + "'></div>";
    // Width
    html += "<div class='form-row'><label>Width (px):</label><input name='width' type='number' min='1' max='64' value='" + String(s.width) + "'></div>";
    // Inner/Outer radii
    html += "<div class='form-row'><label>Inner radius (px):</label><input name='inner' type='number' min='0' max='800' value='" + String(s.inner) + "'></div>";
    html += "<div class='form-row'><label>Outer radius (px):</label><input name='outer' type='number' min='0' max='800' value='" + String(s.outer) + "'></div>";
    // Center X/Y
    html += "<div class='form-row'><label>Center X:</label><input name='cx' type='number' min='0' max='1000' value='" + String(s.cx) + "'> - (Default 240)</div>";
    html += "<div class='form-row'><label>Center Y:</label><input name='cy' type='number' min='0' max='1000' value='" + String(s.cy) + "'> - (Default 240)</div>";
    // Rounded / gradient / foreground
    html += "<div class='form-row'><label>Rounded ends:</label><input name='rounded' type='checkbox'" + String(s.rounded?" checked":"") + "></div>";
    html += "<div class='form-row'><label>Foreground:</label><input name='fg' type='checkbox'" + String(s.foreground?" checked":"") + "></div>";

    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save & Preview</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_save_needles() {
    if (config_server.method() != HTTP_POST) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    int screen = config_server.arg("screen").toInt();
    int gauge = config_server.arg("gauge").toInt();
    if (screen < 0) screen = 0; if (screen >= NUM_SCREENS) screen = 0;
    if (gauge < 0 || gauge > 1) gauge = 0;
    String color = config_server.hasArg("color") ? config_server.arg("color") : String("#FFFFFF");
    int width = config_server.hasArg("width") ? config_server.arg("width").toInt() : 8;
    int inner = config_server.hasArg("inner") ? config_server.arg("inner").toInt() : 142;
    int outer = config_server.hasArg("outer") ? config_server.arg("outer").toInt() : 200;
    int cx = config_server.hasArg("cx") ? config_server.arg("cx").toInt() : 240;
    int cy = config_server.hasArg("cy") ? config_server.arg("cy").toInt() : 240;
    bool rounded = config_server.hasArg("rounded");
    bool gradient = config_server.hasArg("gradient");
    bool fg = config_server.hasArg("fg");

    // clamp sensible ranges
    if (width < 1) width = 1; if (width > 64) width = 64;
    if (inner < 0) inner = 0; if (inner > 2000) inner = 2000;
    if (outer < 0) outer = 0; if (outer > 2000) outer = 2000;
    if (cx < 0) cx = 0; if (cx > 2000) cx = 2000;
    if (cy < 0) cy = 0; if (cy > 2000) cy = 2000;

    save_needle_style_from_args(screen, gauge, color, (uint16_t)width, (int16_t)inner, (int16_t)outer, (uint16_t)cx, (uint16_t)cy, rounded, gradient, fg);

    // Apply immediately
    apply_all_needle_styles();

    // Redirect back to needles page for the same screen/gauge
    String redirect = "/needles?screen=" + String(screen) + "&gauge=" + String(gauge);
    config_server.sendHeader("Location", redirect, true);
    config_server.send(302, "text/plain", "");
}


void setup_network() {
    Serial.begin(115200);
    delay(100);
    Serial.printf("Flash size (ESP.getFlashChipSize()): %u bytes\n", ESP.getFlashChipSize());
    if (!SPIFFS.begin(true)) {
        Serial.println("[ERROR] SPIFFS Mount Failed");
    }
    // Note: Do not load preferences here; caller should load before UI init when required.
    // WiFi connect or AP fallback
    WiFi.mode(WIFI_STA);
    // If a hostname is configured, set it before connecting so DHCP uses it
    if (saved_hostname.length() > 0) {
        WiFi.setHostname(saved_hostname.c_str());
        Serial.println("[WiFi] Hostname set to: " + saved_hostname);
    }
    WiFi.begin(saved_ssid.c_str(), saved_password.c_str());
    Serial.print("Connecting to WiFi");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 30) {
        delay(500);
        Serial.print(".");
        tries++;
        // Silence buzzer during WiFi wait — setup() may have left BEE_EN/PIN6
        // HIGH if the I2C expander direction write failed after a crash-reboot.
        if (is_board_v4()) {
            uint8_t cur_dir;
            if (I2C_Read_EXIO_safe(exio_dir_reg(), &cur_dir) && cur_dir != CH32V003_DIR_SAFE) {
                I2C_Write_EXIO(exio_dir_reg(), CH32V003_DIR_SAFE);
            }
        } else {
            Set_EXIOS(Read_EXIOS(exio_output_reg()) & (uint8_t)~(1 << (EXIO_PIN6 - 1)));
            Mode_EXIOS(0x00);
        }
    }
    if (WiFi.status() == WL_CONNECTED) {
        // Disable power-save so the radio stays awake between loop() calls.
        WiFi.setSleep(false);
        Serial.println("\nWiFi connected!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        // Start mDNS responder so device can be reached by hostname.local
        if (saved_hostname.length() > 0) {
            if (MDNS.begin(saved_hostname.c_str())) {
                Serial.println("[mDNS] Responder started for: " + saved_hostname + ".local");
            } else {
                Serial.println("[mDNS] Failed to start mDNS responder");
            }
        }
    } else {
        Serial.println("\nWiFi failed, starting AP mode");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32-SquareDisplay", "12345678");
        // Disable modem sleep in AP mode — keeps the radio awake for pings
        // and web page requests (avoids intermittent packet loss).
        WiFi.setSleep(false);
        Serial.print("AP IP: ");
        Serial.println(WiFi.softAPIP());
    }
    // Show fallback error screen if needed (after config load, before UI init)
    show_fallback_error_screen_if_needed();

    // Register web UI routes and start server
    config_server.on("/", handle_root);
    config_server.on("/gauges", handle_gauges_page);
    config_server.on("/gauges/ping", []() {
        g_config_page_last_seen = millis();
        config_server.send(204);
    });
    config_server.on("/gauges/close", []() {
        Serial.println("[CFG] Config page closed (explicit), resuming");
        g_config_page_last_seen = 0;
        if (is_signalk_ws_paused() && !g_signalk_ws_resume_pending)
            resume_signalk_ws();
        resume_n2k_updates();
        config_server.send(204);
    });
    config_server.on("/gauges/screen", handle_gauges_screen);
    config_server.on("/gauges/switch", []() {
        // Lightweight endpoint: switch physical display to requested screen.
        // Called by showScreenTab() on every tab click, even when the fragment
        // is served from the JS cache and no full /gauges/screen request is made.
        int s = config_server.arg("s").toInt();
        if (s >= 0 && s < NUM_SCREENS) {
            g_config_page_last_seen = millis();
            ui_set_screen(s + 1);  // 1-based
        }
        config_server.send(204);
    });
    config_server.on("/save-gauges", HTTP_POST, handle_save_gauges);
    config_server.on("/needles", handle_needles_page);
    config_server.on("/save-needles", HTTP_POST, handle_save_needles);
    // Assets manager page and upload/delete handlers
    config_server.on("/assets", handle_assets_page);
    config_server.on("/assets/upload", HTTP_POST, handle_assets_upload_post, handle_assets_upload);
    config_server.on("/assets/delete", HTTP_POST, handle_assets_delete);
    config_server.on("/network", handle_network_page);
    config_server.on("/save-wifi", HTTP_POST, handle_save_wifi);
    config_server.on("/scan-wifi", HTTP_GET, handle_scan_wifi);
    config_server.on("/scan-wifi-results", HTTP_GET, handle_scan_wifi_results);
    config_server.on("/device", handle_device_page);
    config_server.on("/save-device", HTTP_POST, handle_save_device);
    config_server.on("/test-gauge", HTTP_POST, handle_test_gauge);
    config_server.on("/toggle-test-mode", HTTP_POST, handle_toggle_test_mode);
    config_server.on("/set-screen", handle_set_screen);
    config_server.on("/nvs_test", HTTP_GET, handle_nvs_test);
    config_server.on("/update", HTTP_GET,  handle_ota_page);
    config_server.on("/update", HTTP_POST, handle_ota_post, handle_ota_upload);
    config_server.on("/api/imu_calibrate", HTTP_POST, []() {
        attitude_calibrate_level();
        config_server.send(200, "text/plain", "Level set — current N2K attitude saved as zero reference.");
    });
    config_server.on("/api/imu_clear", HTTP_POST, []() {
        attitude_clear_calibration();
        config_server.send(200, "text/plain", "Level cleared — now using raw N2K attitude data.");
    });
    config_server.begin();
    Serial.println("[WebServer] Configuration web UI started on port 80");
    // handleClient() is called from loop() on Core 1.
    // Do NOT run a Core 0 task for this: the WiFi stack also runs on Core 0 and
    // races on internal WiFiClient state, causing LoadProhibited crashes in
    // WiFiClientRxBuffer::read(). Core 1 is safe because WiFi is Core 0-only.

    // Cache the SD asset list now, before any HTTP requests arrive.
    // This avoids running the SD scan inside the HTTP handler where SD/WiFi
    // DMA contention on ESP32-S3 causes the SK WebSocket to drop.
    scan_sd_assets();
}

bool is_wifi_connected() {
    return WiFi.status() == WL_CONNECTED;
}

String get_signalk_server_ip() {
    return saved_signalk_ip;
}

uint16_t get_signalk_server_port() {
    return saved_signalk_port;
}


String get_signalk_path_by_index(int idx) {
    if (idx >= 0 && idx < NUM_SCREENS * 2) return signalk_paths[idx];
    return "";
}

// Get SignalK paths needed by a single screen (0-based index)
std::vector<String> get_signalk_paths_for_screen(int s) {
    std::vector<String> paths;
    std::set<String> unique;
    if (s < 0 || s >= NUM_SCREENS) return paths;

    auto add = [&](const char* p) {
        String ps(p);
        if (ps.length() > 0 && unique.find(ps) == unique.end()) {
            unique.insert(ps);
            paths.push_back(ps);
        }
    };

    // Gauge paths (top = s*2, bottom = s*2+1)
    add(signalk_paths[s * 2].c_str());
    add(signalk_paths[s * 2 + 1].c_str());

    switch (screen_configs[s].display_type) {
        case DISPLAY_TYPE_GAUGE:
            break;
        case DISPLAY_TYPE_NUMBER:
            add(screen_configs[s].number_path);
            break;
        case DISPLAY_TYPE_DUAL:
            add(screen_configs[s].dual_top_path);
            add(screen_configs[s].dual_bottom_path);
            break;
        case DISPLAY_TYPE_QUAD:
            add(screen_configs[s].quad_tl_path);
            add(screen_configs[s].quad_tr_path);
            add(screen_configs[s].quad_bl_path);
            add(screen_configs[s].quad_br_path);
            break;
        case DISPLAY_TYPE_GAUGE_NUMBER:
            add(screen_configs[s].gauge_num_center_path);
            break;
        case DISPLAY_TYPE_GRAPH:
            add(screen_configs[s].number_path);  // primary series uses number_path
            add(screen_configs[s].graph_path_2);
            break;
        case DISPLAY_TYPE_COMPASS:
            add(screen_configs[s].number_path);  // heading path
            add(screen_configs[s].quad_bl_path); // BL extra field
            add(screen_configs[s].quad_br_path); // BR extra field
            break;
        case DISPLAY_TYPE_POSITION:
            add("navigation.position");
            add("navigation.datetime");
            break;
        case DISPLAY_TYPE_AIS:
            add("navigation.position");
            add("navigation.datetime");
            add("navigation.courseOverGroundTrue");
            add("navigation.speedOverGround");
            break;
    }
    return paths;
}

// Get all configured SignalK paths including gauges, number displays, and dual displays
// Returns unique paths only
std::vector<String> get_all_signalk_paths() {
    std::vector<String> all_paths;
    std::set<String> unique_paths;
    
    // Add gauge paths
    for (int i = 0; i < NUM_SCREENS * 2; i++) {
        String path = signalk_paths[i];
        if (path.length() > 0 && unique_paths.find(path) == unique_paths.end()) {
            unique_paths.insert(path);
            all_paths.push_back(path);
        }
    }
    
    // Add number display paths
    for (int s = 0; s < NUM_SCREENS; s++) {
        String path = String(screen_configs[s].number_path);
        if (path.length() > 0 && unique_paths.find(path) == unique_paths.end()) {
            unique_paths.insert(path);
            all_paths.push_back(path);
        }
    }
    
    // Add dual display paths (top and bottom)
    for (int s = 0; s < NUM_SCREENS; s++) {
        String top_path = String(screen_configs[s].dual_top_path);
        if (top_path.length() > 0 && unique_paths.find(top_path) == unique_paths.end()) {
            unique_paths.insert(top_path);
            all_paths.push_back(top_path);
        }
        
        String bottom_path = String(screen_configs[s].dual_bottom_path);
        if (bottom_path.length() > 0 && unique_paths.find(bottom_path) == unique_paths.end()) {
            unique_paths.insert(bottom_path);
            all_paths.push_back(bottom_path);
        }
    }
    
    // Add quad display paths (TL, TR, BL, BR)
    for (int s = 0; s < NUM_SCREENS; s++) {
        String tl_path = String(screen_configs[s].quad_tl_path);
        if (tl_path.length() > 0 && unique_paths.find(tl_path) == unique_paths.end()) {
            unique_paths.insert(tl_path);
            all_paths.push_back(tl_path);
        }
        
        String tr_path = String(screen_configs[s].quad_tr_path);
        if (tr_path.length() > 0 && unique_paths.find(tr_path) == unique_paths.end()) {
            unique_paths.insert(tr_path);
            all_paths.push_back(tr_path);
        }
        
        String bl_path = String(screen_configs[s].quad_bl_path);
        if (bl_path.length() > 0 && unique_paths.find(bl_path) == unique_paths.end()) {
            unique_paths.insert(bl_path);
            all_paths.push_back(bl_path);
        }
        
        String br_path = String(screen_configs[s].quad_br_path);
        if (br_path.length() > 0 && unique_paths.find(br_path) == unique_paths.end()) {
            unique_paths.insert(br_path);
            all_paths.push_back(br_path);
        }
    }
    
    // Add gauge+number display center paths
    for (int s = 0; s < NUM_SCREENS; s++) {
        String center_path = String(screen_configs[s].gauge_num_center_path);
        if (center_path.length() > 0 && unique_paths.find(center_path) == unique_paths.end()) {
            unique_paths.insert(center_path);
            all_paths.push_back(center_path);
        }
    }
    
    // Add graph display second series paths
    for (int s = 0; s < NUM_SCREENS; s++) {
        String graph_path_2 = String(screen_configs[s].graph_path_2);
        if (graph_path_2.length() > 0 && unique_paths.find(graph_path_2) == unique_paths.end()) {
            unique_paths.insert(graph_path_2);
            all_paths.push_back(graph_path_2);
        }
    }

    // Add navigation.position and navigation.datetime if any screen uses Position or AIS display
    {
        bool needs_nav = false;
        bool needs_cog_sog = false;
        for (int s = 0; s < NUM_SCREENS; s++) {
            if (screen_configs[s].display_type == DISPLAY_TYPE_POSITION) needs_nav = true;
            if (screen_configs[s].display_type == DISPLAY_TYPE_AIS) { needs_nav = true; needs_cog_sog = true; }
        }
        if (needs_nav) {
            String nav_pos = "navigation.position";
            if (unique_paths.find(nav_pos) == unique_paths.end()) {
                unique_paths.insert(nav_pos);
                all_paths.push_back(nav_pos);
            }
            String nav_dt = "navigation.datetime";
            if (unique_paths.find(nav_dt) == unique_paths.end()) {
                unique_paths.insert(nav_dt);
                all_paths.push_back(nav_dt);
            }
        }
        if (needs_cog_sog) {
            String nav_cog = "navigation.courseOverGroundTrue";
            if (unique_paths.find(nav_cog) == unique_paths.end()) {
                unique_paths.insert(nav_cog);
                all_paths.push_back(nav_cog);
            }
            String nav_sog = "navigation.speedOverGround";
            if (unique_paths.find(nav_sog) == unique_paths.end()) {
                unique_paths.insert(nav_sog);
                all_paths.push_back(nav_sog);
            }
        }
    }

    return all_paths;
}

void handle_test_gauge() {
    if (config_server.method() == HTTP_POST) {
        int screen = config_server.arg("screen").toInt();
        int gauge = config_server.arg("gauge").toInt();
        int point = config_server.arg("point").toInt();
        int angle = config_server.hasArg("angle") ? config_server.arg("angle").toInt() : gauge_cal[screen][gauge][point].angle;
        extern void test_move_gauge(int screen, int gauge, int angle);
        extern bool test_mode;
        test_mode = true;
        test_move_gauge(screen, gauge, angle);
        // Respond with 204 No Content so the UI does not change
        config_server.send(204, "text/plain", "");
    } else {
        config_server.send(405, "text/plain", "Method Not Allowed");
    }
}

void handle_set_screen() {
    if (config_server.method() == HTTP_GET) {
        int s = config_server.arg("screen").toInt();
        if (s < 1 || s > NUM_SCREENS) s = 1;
        // Call UI C API to change screen (1-5)
        ui_set_screen(s);
        // Redirect back to root so web UI reflects current screen
        config_server.sendHeader("Location", "/", true);
        config_server.send(302, "text/plain", "");
        return;
    }
    config_server.send(405, "text/plain", "Method Not Allowed");
}

void handle_nvs_test() {
    if (config_server.method() != HTTP_GET) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    String resp = "";
    esp_err_t err;
    nvs_handle_t nh;
    uint8_t blob[4] = { 0x12, 0x34, 0x56, 0x78 };
    err = nvs_open(PREF_NAMESPACE, NVS_READWRITE, &nh);
    Serial.printf("[NVS TEST] nvs_open -> %s (%d)\n", esp_err_to_name(err), err);
    resp += String("nvs_open: ") + (err == ESP_OK ? esp_err_to_name(err) : String(err)) + "\n";
    if (err == ESP_OK) {
        err = nvs_set_blob(nh, "test_blob", blob, sizeof(blob));
        Serial.printf("[NVS TEST] nvs_set_blob -> %s (%d)\n", esp_err_to_name(err), err);
        resp += String("nvs_set_blob: ") + (err == ESP_OK ? esp_err_to_name(err) : String(err)) + "\n";
        err = nvs_commit(nh);
        Serial.printf("[NVS TEST] nvs_commit -> %s (%d)\n", esp_err_to_name(err), err);
        resp += String("nvs_commit: ") + (err == ESP_OK ? esp_err_to_name(err) : String(err)) + "\n";

        uint8_t readbuf[4] = {0,0,0,0};
        size_t rsz = sizeof(readbuf);
        err = nvs_get_blob(nh, "test_blob", readbuf, &rsz);
        Serial.printf("[NVS TEST] nvs_get_blob -> %s (%d) size=%u\n", esp_err_to_name(err), err, (unsigned)rsz);
        resp += String("nvs_get_blob: ") + (err == ESP_OK ? esp_err_to_name(err) : String(err)) + " size=" + String(rsz) + "\n";
        if (err == ESP_OK) {
            char bstr[64];
            snprintf(bstr, sizeof(bstr), "read: %02X %02X %02X %02X\n", readbuf[0], readbuf[1], readbuf[2], readbuf[3]);
            Serial.printf("[NVS TEST] read bytes: %02X %02X %02X %02X\n", readbuf[0], readbuf[1], readbuf[2], readbuf[3]);
            resp += String(bstr);
        }
        nvs_close(nh);
    }
    config_server.send(200, "text/plain", resp);
}

// Assets manager: list files and show upload form
void handle_assets_page() {
    // Use cached file list — avoids SD scan during HTTP handling (SD/WiFi DMA conflict).
    // Merged icon + bg into a single list for display.
    config_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    config_server.send(200, "text/html; charset=utf-8", "");
    String html;
    html.reserve(8192);  // >4096 → PSRAM via SPIRAM threshold
    auto flush = [&]() {
        if (html.length() > 0) {
            config_server.sendContent(html);
            html.clear();
            // Do NOT call Lvgl_Loop() here — re-entrant heap corruption.
        }
    };
    html = "<!DOCTYPE html><html><head>";
    html += STYLE;
    html += "<title>Assets Manager</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Assets Manager</h2>";
    // Upload form (styled)
    html += "<div class='assets-uploader'><form method='POST' action='/assets/upload' enctype='multipart/form-data' style='display:flex;gap:8px;align-items:center;'>";
    html += "<input type='file' name='file' accept='image/png,image/jpeg,image/bmp,image/gif'>";
    html += "<input type='submit' value='Upload' class='tab-btn'>";
    html += "</form></div>";
    flush();
    html += "<h3>Files in /assets</h3>";
    html += "<table class='file-table'><tr><th>Name</th><th>Actions</th></tr>";
    // Use cached file list — no SD access during HTTP handling
    auto addRow = [&](const String& path) {
        // path is like "S://assets/foo.bin" — extract basename
        String bname = path;
        int sl = bname.lastIndexOf('/');
        if (sl >= 0) bname = bname.substring(sl + 1);
        String sdpath = String("/assets/") + bname;
        html += "<tr><td>" + bname + "</td>";
        html += "<td class='file-actions'><form method='POST' action='/assets/delete'><input type='hidden' name='file' value='" + bname + "'>";
        html += "<input type='submit' value='Delete' class='tab-btn' onclick='return confirm(\"Delete " + bname + "?\")'></form>";
        html += " <a href='S:" + sdpath + "' target='_blank' class='tab-btn' style='padding:6px 10px;text-decoration:none;'>Download</a></td></tr>";
        flush();
    };
    for (const auto& f : g_bgFiles)   addRow(f);
    for (const auto& f : g_iconFiles) addRow(f);
    html += "</table>";
    html += "<p style='text-align:center; margin-top:12px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    flush();
    config_server.sendContent(""); // terminate chunked transfer encoding
}

// Upload handler: called during multipart upload
static File assets_upload_file;
// POSIX FILE* fallback when `SD_MMC.open` cannot open the desired path
static FILE *assets_upload_fp = NULL;
void handle_assets_upload() {
    HTTPUpload& upload = config_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        // sanitize filename: remove paths
        int slash = filename.lastIndexOf('/');
        if (slash >= 0) filename = filename.substring(slash + 1);
        String path = String("/assets/") + filename;
        Serial.printf("[ASSETS] Upload start: %s -> %s\n", upload.filename.c_str(), path.c_str());
        // open file for write (overwrite)
        assets_upload_file = SD_MMC.open(path, FILE_WRITE);
        if (!assets_upload_file) {
            Serial.printf("[ASSETS] SD_MMC open failed for %s, trying POSIX fallback\n", path.c_str());
            // Try POSIX fopen on /sdcard prefix (SDSPI mount uses /sdcard)
            String alt = String("/sdcard") + path;
            assets_upload_fp = fopen(alt.c_str(), "wb");
            if (!assets_upload_fp) {
                Serial.printf("[ASSETS] POSIX fopen fallback failed for %s\n", alt.c_str());
            } else {
                Serial.printf("[ASSETS] POSIX fopen fallback opened %s\n", alt.c_str());
            }
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (assets_upload_file) {
            assets_upload_file.write(upload.buf, upload.currentSize);
        } else if (assets_upload_fp) {
            fwrite(upload.buf, 1, upload.currentSize, assets_upload_fp);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (assets_upload_file) {
            assets_upload_file.flush();
            assets_upload_file.close();
            Serial.printf("[ASSETS] Upload finished (SD_MMC): %s (%u bytes)\n", upload.filename.c_str(), (unsigned)upload.totalSize);
        } else if (assets_upload_fp) {
            fclose(assets_upload_fp);
            assets_upload_fp = NULL;
            Serial.printf("[ASSETS] Upload finished (POSIX fallback): %s (%u bytes)\n", upload.filename.c_str(), (unsigned)upload.totalSize);
        }
    }
}

// Final POST handler after upload completes (redirect back)
void handle_assets_upload_post() {
    scan_sd_assets(); // refresh cached file list after new upload
    String html = "<!DOCTYPE html><html><head>";
    html += STYLE;
    html += "<title>Upload Complete</title></head><body><div class='container'>";
    html += "<h3>Upload complete</h3>";
    html += "<p><a href='/assets'>Back to Assets</a></p>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_assets_delete() {
    if (config_server.method() != HTTP_POST) { config_server.send(405, "text/plain", "Method Not Allowed"); return; }
    String fname = config_server.arg("file");
    if (fname.length() == 0) { config_server.send(400, "text/plain", "Missing file parameter"); return; }
    // sanitize
    if (fname.indexOf("..") != -1 || fname.indexOf('/') != -1 || fname.indexOf('\\') != -1) {
        config_server.send(400, "text/plain", "Invalid filename"); return;
    }
    String path = String("/assets/") + fname;
    if (SD_MMC.exists(path)) {
        bool ok = SD_MMC.remove(path);
        Serial.printf("[ASSETS] Delete %s -> %d\n", path.c_str(), ok);
        scan_sd_assets(); // refresh cached file list after delete
    }
    // redirect back
    config_server.sendHeader("Location", "/assets");
    config_server.send(303, "text/plain", "");
}

// ---------------------------------------------------------------------------
// OTA firmware update handlers
// ---------------------------------------------------------------------------
void handle_ota_upload() {
    HTTPUpload& upload = config_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
        // UPDATE_SIZE_UNKNOWN lets the Update library size from the partition table
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        esp_task_wdt_reset();
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        esp_task_wdt_reset();   // end() verifies hash — can take a moment
        if (Update.end(true)) {
            Serial.printf("[OTA] Success: %u bytes\n",
                          (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void handle_ota_post() {
    bool ok = !Update.hasError();
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
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
        // Flush TCP before we kill the radio — client must receive the page first.
        config_server.client().flush();
        delay(200);

        // Turn off backlight and stop the display before restarting.
        // On ESP32-S3 the RGB panel uses continuous GDMA; if a DMA transfer
        // is in-flight when esp_restart() is called, the hardware can hang
        // and the restart never completes — visible as the device freezing
        // with the screen still lit after an OTA upload.
        extern void Set_Backlight(uint8_t);
        Set_Backlight(0);
        extern esp_lcd_panel_handle_t panel_handle;
        if (panel_handle) esp_lcd_panel_disp_on_off(panel_handle, false);
        delay(200);

        Serial.println("[OTA] Restarting...");
        Serial.flush();
        esp_restart();
    }
}

void handle_ota_page() {
    // Show running partition and free space so the user can sanity-check
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
    html += "<p>Select a <code>.bin</code> firmware file built for this board:</p>";
    html += "<input type='file' name='firmware' accept='.bin' required style='margin-bottom:12px'><br>";
    html += "<input type='submit' value='Upload &amp; Flash' "
            "onclick=\"this.disabled=true;this.value='Flashing&hellip;';this.form.submit()\">";
    html += "</form>";
    html += "<p style='margin-top:20px'><a href='/'>&#8592; Back</a></p>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}
