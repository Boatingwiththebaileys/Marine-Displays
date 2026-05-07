// ...existing code...

#include "gauge_config.h"
#include "version.h"
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
#include "ui_Settings.h"
#include <FS.h>
#include <SPIFFS.h>
#include <SD_MMC.h>

// ...existing code...

// Place fallback/error screen logic after all includes and config loads
extern "C" void show_fallback_error_screen_if_needed() {
    bool all_default = true;
    for (int s = 0; s < NUM_SCREENS && all_default; ++s) {
        for (int g = 0; g < 2 && all_default; ++g) {
            for (int p = 0; p < 5; ++p) {
                if (screen_configs[s].cal[g][p].angle != 0 || screen_configs[s].cal[g][p].value != 0.0f) {
                    all_default = false; break;
                }
            }
        }
        // Also check display_type, icon_paths, background_path
        if (all_default) {
            if (screen_configs[s].display_type != 0 ||
                screen_configs[s].icon_paths[0][0] != '\0' ||
                screen_configs[s].icon_paths[1][0] != '\0' ||
                screen_configs[s].background_path[0] != '\0') {
                all_default = false;
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
#include "gauge_config.h"
#include "screen_config_c_api.h"
#include <FS.h>
#include <SPIFFS.h>
#include <SD_MMC.h>

#include "nvs.h"
#include "nvs_flash.h"
#include <esp_err.h>
#include "esp_log.h"
#include "needle_style.h"

static const char *TAG_SETUP = "network_setup";

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

// Minimal HTML style used by the configuration web pages
const String STYLE = R"rawliteral(
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

</style>
)rawliteral";

// Forward declaration for toggle test mode handler
void handle_toggle_test_mode();
void handle_test_gauge();
void handle_nvs_test();
void handle_set_screen();
void handle_gauges_screen();
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
// 10 SignalK paths: [screen][gauge] => idx = s*2+g
String signalk_paths[NUM_SCREENS * 2];
// Auto-scroll interval in seconds (0 = off)
uint16_t auto_scroll_sec = 0;
// Skip a single load of preferences when we've just saved, so the UI
// reflects the in-memory `screen_configs` we just updated instead of
// reloading possibly-stale NVS values.
static volatile bool skip_next_load_preferences = false;
// Namespaces used for Preferences / NVS
const char* SETTINGS_NAMESPACE = "settings";
const char* PREF_NAMESPACE = "gaugeconfig";

// Asset file lists — populated once at startup by scan_sd_assets().
// Reusing these avoids a live SD scan during HTTP handling which causes
// SD/WiFi DMA contention on ESP32-S3.
static std::vector<String> g_iconFiles;
static std::vector<String> g_bgFiles;

// Single shared HTML buffer for handle_gauges_page() and handle_gauges_screen().
String g_http_html_buf;
bool   g_http_html_buf_reserved = false;

static void scan_sd_assets() {
    g_iconFiles.clear();
    g_bgFiles.clear();
    File root = SD_MMC.open("/assets");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            String fname = file.name();
            file = root.openNextFile();
            if (fname.startsWith("._") || fname.startsWith("_")) continue;
            String lname = fname;
            lname.toLowerCase();
            String fullPath = fname.startsWith("/assets/") ? fname : "/assets/" + fname;
            if (lname.endsWith(".png"))      g_iconFiles.push_back(String("S:/") + fullPath);
            else if (lname.endsWith(".bin")) g_bgFiles.push_back(String("S:/") + fullPath);
        }
    }
    Serial.printf("[ASSET SCAN] Cached %u bg files, %u icon files\n",
                  (unsigned)g_bgFiles.size(), (unsigned)g_iconFiles.size());
}

// Ensure ScreenConfig/screen_configs symbol visible
#include "screen_config_c_api.h"

// Expose runtime settings from other modules
extern int buzzer_mode;
extern uint16_t buzzer_cooldown_sec;
extern bool first_run_buzzer;
extern uint8_t LCD_Backlight;
// UI control helpers (implemented in ui.c)
extern "C" int ui_get_current_screen(void);
extern "C" void ui_set_screen(int screen_num);

void save_preferences() {
    Serial.println("[DEBUG] Saving preferences...");
    preferences.end();
    if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
        Serial.println("[ERROR] preferences.begin failed for settings namespace");
    } else {
        preferences.putString("ssid", saved_ssid);
        preferences.putString("password", saved_password);
        preferences.putString("signalk_ip", saved_signalk_ip);
        preferences.putString("hostname", saved_hostname);
        preferences.putUShort("signalk_port", saved_signalk_port);
        // Persist device settings
        preferences.putUShort("buzzer_mode", (uint16_t)buzzer_mode);
        preferences.putUShort("buzzer_cooldown", buzzer_cooldown_sec);
        preferences.putShort("bright_pos", (int16_t)brightness_slider_pos);
        // Save auto-scroll setting
        preferences.putUShort("auto_scroll", auto_scroll_sec);
        preferences.putUShort("unit_system", (uint16_t)unit_system);
        for (int i = 0; i < NUM_SCREENS * 2; ++i) {
            String key = String("skpath_") + i;
            preferences.putString(key.c_str(), signalk_paths[i]);
        }
        preferences.end();
    }

    // Try to save per-screen blobs via NVS
    nvs_handle_t nvs_handle;
    esp_err_t nvs_err = nvs_open(PREF_NAMESPACE, NVS_READWRITE, &nvs_handle);
    bool any_nvs_ok = false;
    bool nvs_invalid_length_detected = false;
    const size_t CHUNK_SIZE = 128;
    if (nvs_err == ESP_OK) {
        for (int s = 0; s < NUM_SCREENS; ++s) {
            // copy runtime calibration into screen_configs
            for (int g = 0; g < 2; ++g) for (int p = 0; p < 5; ++p) screen_configs[s].cal[g][p] = gauge_cal[s][g][p];
            char key[32];
            snprintf(key, sizeof(key), "screen%d", s);
            esp_err_t err = nvs_set_blob(nvs_handle, key, &screen_configs[s], sizeof(ScreenConfig));
            Serial.printf("[NVS SAVE] nvs_set_blob('%s', size=%u) -> %d\n", key, (unsigned)sizeof(ScreenConfig), err);
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
                Serial.printf("[NVS SAVE] nvs_set_blob('%s', size=%u) -> %d\n", key, (unsigned)part_sz, perr);
                if (perr != ESP_OK) { parts_ok = false; break; }
            }
            if (parts_ok) {
                any_nvs_ok = true;
                Serial.printf("[NVS SAVE] Chunked write succeeded for screen%d (%d parts)\n", s, parts);
            }
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    } else {
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
                // Restore ALL settings (not just WiFi — also device prefs)
                if (preferences.begin(SETTINGS_NAMESPACE, false)) {
                    preferences.putString("ssid", saved_ssid);
                    preferences.putString("password", saved_password);
                    preferences.putString("signalk_ip", saved_signalk_ip);
                    preferences.putString("hostname", saved_hostname);
                    preferences.putUShort("signalk_port", saved_signalk_port);
                    preferences.putUShort("buzzer_mode", (uint16_t)buzzer_mode);
                    preferences.putUShort("buzzer_cooldown", buzzer_cooldown_sec);
                    preferences.putShort("bright_pos", (int16_t)brightness_slider_pos);
                    preferences.putUShort("auto_scroll", auto_scroll_sec);
                    preferences.putUShort("unit_system", (uint16_t)unit_system);
                    for (int i = 0; i < NUM_SCREENS * 2; ++i) {
                        String key = String("skpath_") + i;
                        preferences.putString(key.c_str(), signalk_paths[i]);
                    }
                    preferences.end();
                    Serial.println("[NVS REPAIR] Restored all settings to NVS");
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

    // Debug: print signalk_paths content before SD/NVS operations
    Serial.println("[DEBUG] signalk_paths before saving:");
    for (int i = 0; i < NUM_SCREENS * 2; ++i) {
        Serial.printf("[DEBUG] signalk_paths[%d] = '%s'\n", i, signalk_paths[i].c_str());
    }

    {
        Serial.println("[SD SAVE] Saving screen configs to SD...");
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        for (int s = 0; s < NUM_SCREENS; ++s) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s);
            File f = SD_MMC.open(sdpath, FILE_WRITE);
            if (!f) { Serial.printf("[SD SAVE] Failed to open '%s'\n", sdpath); continue; }
            size_t written = f.write((const uint8_t *)&screen_configs[s], sizeof(ScreenConfig));
            f.flush();
            f.close();
            Serial.printf("[SD SAVE] Wrote '%s' -> %u bytes\n", sdpath, (unsigned)written);
        }
    }

    // Verify settings namespace saved correctly (read back keys)
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
        Serial.println("[DEBUG] Verifying saved SignalK path keys in SETTINGS_NAMESPACE:");
        for (int i = 0; i < NUM_SCREENS * 2; ++i) {
            String key = String("skpath_") + i;
            String v = preferences.getString(key.c_str(), "<missing>");
            Serial.printf("[DEBUG] prefs[%s] = '%s'\n", key.c_str(), v.c_str());
        }
        String vs = preferences.getString("ssid", "<missing>");
        String vp = preferences.getString("password", "<missing>");
        Serial.printf("[DEBUG] prefs saved SSID='%s' PASSWORD='%s'\n", vs.c_str(), vp.c_str());
        preferences.end();
    } else {
        Serial.println("[DEBUG] preferences.begin(SETTINGS_NAMESPACE, true) failed for verification");
    }

    // Always write SignalK paths to SD as a fallback — OUTSIDE any preferences block
    if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
    File spf = SD_MMC.open("/config/signalk_paths.txt", FILE_WRITE);
    if (spf) {
        for (int i = 0; i < NUM_SCREENS * 2; ++i) {
            spf.println(signalk_paths[i]);
        }
        spf.flush();
        spf.close();
        Serial.println("[SD SAVE] Wrote /config/signalk_paths.txt");
    } else {
        Serial.println("[SD SAVE] Failed to open /config/signalk_paths.txt for writing");
    }
}

// Load preferences and screen configs from NVS or SD fallback
void load_preferences() {
    // Load settings (WiFi, Signalk) from SETTINGS_NAMESPACE
    preferences.end();
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
        saved_ssid = preferences.getString("ssid", "");
        saved_password = preferences.getString("password", "");
        saved_signalk_ip = preferences.getString("signalk_ip", "");
        saved_signalk_port = preferences.getUShort("signalk_port", 0);
        saved_hostname = preferences.getString("hostname", "");
        // Load auto-scroll interval (seconds)
        auto_scroll_sec = preferences.getUShort("auto_scroll", 0);
        unit_system = (UnitSystem)preferences.getUShort("unit_system", (uint16_t)UNIT_NAUTICAL_METRIC);
        // Load device settings
        buzzer_mode = (int)preferences.getUShort("buzzer_mode", (uint16_t)buzzer_mode);
        buzzer_cooldown_sec = preferences.getUShort("buzzer_cooldown", buzzer_cooldown_sec);
        // Mark first run so buzzer logic can re-evaluate immediately
        first_run_buzzer = true;
        // Load unified slider position (0-100); migrate from old "brightness" key if needed
        int16_t slider_pos = preferences.getShort("bright_pos", -1);
        if (slider_pos < 0) {
            // First boot after update: reverse-map old hw brightness → slider position
            uint16_t old_bright = preferences.getUShort("brightness", 50);
            if (old_bright < 10) old_bright = 10;
            if (old_bright > 100) old_bright = 100;
            slider_pos = 50 + ((int)old_bright - 10) * 50 / 90;
        }
        if (slider_pos > 100) slider_pos = 100;
        if (slider_pos < 0) slider_pos = 0;
        brightness_slider_pos = (uint8_t)slider_pos;
        // Compute hw brightness from slider position (no overlay yet — LVGL not initialized)
        if (brightness_slider_pos >= 50)
            LCD_Backlight = (uint8_t)(10 + (brightness_slider_pos - 50) * 90 / 50);
        else
            LCD_Backlight = (uint8_t)(1 + brightness_slider_pos * 9 / 49);
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
    Serial.printf("[DEBUG] Loaded settings: ssid='%s' password='%s' signalk_ip='%s' port=%u\n",
                  saved_ssid.c_str(), saved_password.c_str(), saved_signalk_ip.c_str(), saved_signalk_port);

    // Initialize defaults
    for (int s = 0; s < NUM_SCREENS; ++s) {
        // zero screen_configs so defaults are predictable
        memset(&screen_configs[s], 0, sizeof(ScreenConfig));
        // sensible defaults for icon positions: top icon -> top (0), bottom icon -> bottom (2)
        screen_configs[s].icon_pos[0] = 0;
        screen_configs[s].icon_pos[1] = 2;
        // default to showing bottom gauge
        screen_configs[s].show_bottom = 1;
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

    // If any screen config is still default, always restore from SD if available
    bool restored_from_sd = false;
    for (int s = 0; s < NUM_SCREENS; ++s) {
        bool is_default = true;
        // Check calibration data
        for (int g = 0; g < 2 && is_default; ++g) {
            for (int p = 0; p < 5; ++p) {
                if (screen_configs[s].cal[g][p].angle != 0 || screen_configs[s].cal[g][p].value != 0.0f) {
                    is_default = false; break;
                }
            }
        }
        // Also check display_type, icon_paths, and background_path
        if (is_default) {
            if (screen_configs[s].display_type != 0 ||
                screen_configs[s].icon_paths[0][0] != '\0' ||
                screen_configs[s].icon_paths[1][0] != '\0' ||
                screen_configs[s].background_path[0] != '\0') {
                is_default = false;
            }
        }
        if (is_default) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s);
            if (SD_MMC.exists(sdpath)) {
                File f = SD_MMC.open(sdpath, FILE_READ);
                if (f) {
                    memset(&screen_configs[s], 0, sizeof(ScreenConfig)); // zero-fill first for forward-compat
                    size_t got = f.read((uint8_t *)&screen_configs[s], sizeof(ScreenConfig));
                    Serial.printf("[SD LOAD] Read '%s' -> %u bytes (expected %u)\n", sdpath, (unsigned)got, (unsigned)sizeof(ScreenConfig));
                    f.close();
                    if (got > sizeof(ScreenConfig) || got == 0) {
                        Serial.printf("[SD LOAD] Invalid size for '%s', restoring defaults\n", sdpath);
                        memset(&screen_configs[s], 0, sizeof(ScreenConfig));
                        continue;
                    }
                    if (got < sizeof(ScreenConfig)) {
                        Serial.printf("[SD LOAD] Migrating old config '%s' (%u -> %u bytes), new fields zeroed\n", sdpath, (unsigned)got, (unsigned)sizeof(ScreenConfig));
                    }
                    // Validate loaded config
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
                    Serial.printf("[SD LOAD] Failed to open '%s', restoring defaults for screen %d\n", sdpath, s);
                    memset(&screen_configs[s], 0, sizeof(ScreenConfig));
                }
            } else {
                Serial.printf("[CONFIG ERROR] No SD config for screen %d, restoring defaults\n", s);
                memset(&screen_configs[s], 0, sizeof(ScreenConfig));
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

void handle_gauges_page() {
    // Lightweight AJAX shell — per-screen config loaded on demand via
    // /gauges/screen?s=N.  Keeps peak HTML under ~4 KB.
    skip_next_load_preferences = false;

    // Reload asset lists so newly-uploaded files appear
    scan_sd_assets();

    extern bool test_mode;

    config_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    config_server.send(200, "text/html; charset=utf-8", "");

    String& html = g_http_html_buf;
    if (!g_http_html_buf_reserved) {
        html.reserve(4096);
        g_http_html_buf_reserved = true;
    }
    html.clear();
    extern void Lvgl_Loop(void);
    auto flushHtml = [&]() {
        if (html.length() > 0) {
            config_server.sendContent(html);
            html.clear();
            Lvgl_Loop();
            vTaskDelay(1);
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
    html += (test_mode ? "Disable Setup Mode" : "Enable Setup Mode");
    html += "</button> ";
    html += "<span id='testModeLabel' style='font-weight:bold;color:";
    html += (test_mode ? "#388e3c;'>SETUP MODE ON" : "#b71c1c;'>SETUP MODE OFF");
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

    // showScreenTab: fetch a single screen's config HTML from the device
    html += "function showScreenTab(idx){\n";
    html += "  if(idx===currentTab) return;\n";
    html += "  currentTab=idx;\n";
    html += "  document.getElementById('save_screen').value=idx;\n";
    html += "  for(var s=0;s<NUM_SCREENS;s++){\n";
    html += "    var b=document.getElementById('tabbtn_'+s);\n";
    html += "    if(b) b.style.background=(s===idx?'#e3eaf6':'#f4f6fa');\n";
    html += "  }\n";
    html += "  var cont=document.getElementById('screen-content');\n";
    html += "  cont.innerHTML='<p style=\"text-align:center;color:#888;padding:40px 0;\">Loading...</p>';\n";
    html += "  fetch('/gauges/screen?s='+idx)\n";
    html += "    .then(function(r){return r.text();})\n";
    html += "    .then(function(h){\n";
    html += "      cont.innerHTML=h;\n";
    html += "      initScreenTab(idx);\n";
    html += "    })\n";
    html += "    .catch(function(e){\n";
    html += "      cont.innerHTML='<p style=\"color:red;text-align:center;\">Failed to load – '+e+'</p>';\n";
    html += "    });\n";
    html += "  try{ history.replaceState && history.replaceState(null,null,'#tab'+idx); }catch(e){}\n";
    html += "}\n";

    // initScreenTab: called after injecting screen HTML — set up toggles
    html += "function initScreenTab(s){\n";
    html += "  toggleGaugeConfig(s);\n";
    html += "}\n";

    // toggleGaugeConfig — show/hide Gauge+Number config section
    html += "function toggleGaugeConfig(screen){\n";
    html += "  var sel=document.querySelector('select[name=\"dtype_'+screen+'\"]');\n";
    html += "  if(!sel) return;\n";
    html += "  var gnDiv=document.getElementById('gnumcfg_'+screen);\n";
    html += "  if(gnDiv) gnDiv.style.display=(sel.value==='4'?'block':'none');\n";
    html += "}\n";

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
    html += "      if(btn)btn.textContent='Disable Setup Mode';\n";
    html += "      if(lbl){lbl.style.color='#388e3c';lbl.textContent='SETUP MODE ON';}\n";
    html += "    } else {\n";
    html += "      if(btn)btn.textContent='Enable Setup Mode';\n";
    html += "      if(lbl){lbl.style.color='#b71c1c';lbl.textContent='SETUP MODE OFF';}\n";
    html += "    }\n";
    html += "    var prev=currentTab; currentTab=-1; showScreenTab(prev>=0?prev:0);\n";
    html += "  }).catch(function(e){console.error(e);});\n";
    html += "}\n";

    // Test gauge point via AJAX
    html += "function testGaugePoint(s,g,p){\n";
    html += "  var ai=document.querySelector('input[name=\"angle_'+s+'_'+g+'_'+p+'\"]');\n";
    html += "  var angle=ai?ai.value:'0';\n";
    html += "  fetch('/test-gauge',{method:'POST',\n";
    html += "    headers:{'Content-Type':'application/x-www-form-urlencoded'},\n";
    html += "    body:'screen='+s+'&gauge='+g+'&point='+p+'&angle='+angle})\n";
    html += "  .catch(function(e){console.error(e);});\n";
    html += "}\n";

    // Load first tab on page load
    html += "document.addEventListener('DOMContentLoaded',function(){\n";
    html += "  var initial=0;\n";
    html += "  if(location.hash&&location.hash.indexOf('#tab')===0) initial=parseInt(location.hash.replace('#tab',''))||0;\n";
    html += "  showScreenTab(initial);\n";
    html += "});\n";
    html += "</script>\n";
    html += "</div></body></html>";
    flushHtml();
    config_server.sendContent(""); // close chunked transfer
}

// Per-screen AJAX fragment — returns HTML for one screen's config
void handle_gauges_screen() {
    int s = config_server.arg("s").toInt();
    if (s < 0 || s >= NUM_SCREENS) s = 0;

    // Reload config from storage so we display current values
    if (!skip_next_load_preferences) {
        load_preferences();
    } else {
        skip_next_load_preferences = false;
    }

    config_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    config_server.send(200, "text/html; charset=utf-8", "");

    String& html = g_http_html_buf;
    if (!g_http_html_buf_reserved) {
        html.reserve(4096);
        g_http_html_buf_reserved = true;
    }
    html.clear();
    extern void Lvgl_Loop(void);
    auto flushHtml = [&]() {
        if (html.length() > 0) {
            config_server.sendContent(html);
            html.clear();
            Lvgl_Loop();
            vTaskDelay(1);
        }
    };

    extern bool test_mode;

    html += "<h3>Screen " + String(s+1) + "</h3>";

    // Display Type selection
    int curDt = screen_configs[s].display_type;
    html += "<div style='margin-bottom:8px;'><label>Display Type: <select name='dtype_" + String(s) + "' onchange='toggleGaugeConfig(" + String(s) + ")'>";
    html += "<option value='0'"; if (curDt == 0) html += " selected"; html += ">Gauge</option>";
    html += "<option value='4'"; if (curDt == 4) html += " selected"; html += ">Gauge + Number</option>";
    html += "</select></label></div>";

    // Gauge+Number config section
    html += "<div id='gnumcfg_" + String(s) + "' style='display:" + (curDt == 4 ? "block" : "none") + "; border:1px solid #ccc; padding:8px; margin-bottom:8px; border-radius:4px;'>";
    html += "<b>Center Number Config</b>";
    html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='gnum_path_" + String(s) + "' type='text' value='" + String(screen_configs[s].gauge_num_center_path) + "' style='width:80%'></label></div>";
    int curFs = screen_configs[s].gauge_num_center_font_size;
    html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='gnum_font_" + String(s) + "'>";
    struct { int v; const char* n; } fsopts[] = {{0,"48pt"},{1,"72pt"},{2,"96pt"},{3,"120pt"},{4,"144pt"}};
    for (int fo = 0; fo < 5; ++fo) {
        html += "<option value='" + String(fsopts[fo].v) + "'";
        if (curFs == fsopts[fo].v) html += " selected";
        html += ">" + String(fsopts[fo].n) + "</option>";
    }
    html += "</select></label></div>";
    html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='gnum_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].gauge_num_center_font_color) + "'></label></div>";
    html += "</div>";

    // Background selection
    String savedBg = String(screen_configs[s].background_path);
    String savedBgNorm = savedBg;
    savedBgNorm.toLowerCase();
    savedBgNorm.replace("S://", "S:/");
    while (savedBgNorm.indexOf("//") != -1) savedBgNorm.replace("//", "/");
    html += "<div style='margin-bottom:8px;'><label>Background: <select name='bg_" + String(s) + "'>";
    html += "<option value=''";
    if (savedBg.length() == 0) html += " selected='selected'";
    html += ">Default</option>";
    for (const auto& b : g_bgFiles) {
        String bNorm = b;
        bNorm.toLowerCase();
        bNorm.replace("S://", "S:/");
        while (bNorm.indexOf("//") != -1) bNorm.replace("//", "/");
        html += "<option value='" + b + "'";
        if (bNorm == savedBgNorm && savedBg.length() > 0) html += " selected='selected'";
        html += ">" + b + "</option>";
    }
    html += "</select></label></div>";
    flushHtml();

    for (int g = 0; g < 2; ++g) {
        // Show Bottom Gauge checkbox (only once per screen)
        if (g == 0) {
            html += "<div style='margin-bottom:8px;'><label>Show Bottom Gauge: <input type='checkbox' name='showbottom_" + String(s) + "'";
            if (screen_configs[s].show_bottom) html += " checked";
            html += "></label></div>";
        }
        int idx = s * 2 + g;
        // skip rendering bottom gauge config if disabled
        if (g == 1 && !screen_configs[s].show_bottom) {
            html += "<div style='margin-bottom:8px;'><em>Bottom gauge disabled for this screen.</em></div>";
            continue;
        }
        html += "<b>" + String(g == 0 ? "Top Gauge" : "Bottom Gauge") + "</b>";
        // SignalK Path
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='skpath_" + String(s) + "_" + String(g) + "' type='text' value='" + signalk_paths[idx] + "' style='width:80%'></label></div>";

        // Calibration points table
        html += "<table class='table'><tr><th>Point</th><th>Angle</th><th>Value</th><th>Test</th></tr>";
        for (int p = 0; p < 5; ++p) {
            html += "<tr><td>" + String(p+1) + "</td>";
            html += "<td><input name='angle_" + String(s) + "_" + String(g) + "_" + String(p) + "' type='number' value='" + String(gauge_cal[s][g][p].angle) + "'></td>";
            html += "<td><input name='value_" + String(s) + "_" + String(g) + "_" + String(p) + "' type='number' step='any' value='" + String(gauge_cal[s][g][p].value) + "'></td>";
            html += "<td><button type='button' onclick='testGaugePoint(" + String(s) + "," + String(g) + "," + String(p) + ")'";
            if (!test_mode) html += " disabled";
            html += ">Test</button></td></tr>";
        }
        html += "</table>";

        // Icon controls
        String savedIcon = String(screen_configs[s].icon_paths[g]);
        String savedIconNorm = savedIcon;
        savedIconNorm.toLowerCase();
        savedIconNorm.replace("S://", "S:/");
        while (savedIconNorm.indexOf("//") != -1) savedIconNorm.replace("//", "/");
        html += "<div class='icon-section'><div class='icon-row'>";
        html += "<div style='margin-bottom:8px;'><label>Icon: <select name='icon_" + String(s) + "_" + String(g) + "'>";
        html += "<option value=''";
        if (savedIcon.length() == 0) html += " selected='selected'";
        html += ">None</option>";
        for (const auto& icon : g_iconFiles) {
            String iconNorm = icon;
            iconNorm.toLowerCase();
            iconNorm.replace("S://", "S:/");
            while (iconNorm.indexOf("//") != -1) iconNorm.replace("//", "/");
            html += "<option value='" + icon + "'";
            if (iconNorm == savedIconNorm && savedIcon.length() > 0) html += " selected='selected'";
            html += ">" + icon + "</option>";
        }
        html += "</select></label></div>";
        // Icon position selector
        int curPos = screen_configs[s].icon_pos[g];
        html += "<div style='margin-bottom:8px;'><label>Icon Position: <select name='iconpos_" + String(s) + "_" + String(g) + "'>";
        struct { int v; const char *n; } posopts[] = { {0,"Top"}, {1,"Right"}, {2,"Bottom"}, {3,"Left"} };
        for (int _po = 0; _po < 4; ++_po) {
            html += "<option value='" + String(posopts[_po].v) + "'";
            if (curPos == posopts[_po].v) html += " selected='selected'";
            html += ">" + String(posopts[_po].n) + "</option>";
        }
        html += "</select></label></div>";
        html += "</div>"; // close icon-row

        // Zone min/max/color/transparent/buzzer controls
        html += "<div class='zone-row'>";
        for (int i = 1; i <= 4; ++i) {
            float minVal = screen_configs[s].min[g][i];
            float maxVal = screen_configs[s].max[g][i];
            String colorVal = String(screen_configs[s].color[g][i]);
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
        html += "</div>";

        html += "</div>"; // close icon-section
        flushHtml(); // flush after each gauge to keep heap usage bounded
    }
    flushHtml();
    config_server.sendContent(""); // close chunked transfer

    // Switch the physical display to this screen
    ui_set_screen(s + 1);
}

void handle_save_gauges() {
    if (config_server.method() != HTTP_POST) {
        config_server.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    // Support saving a single screen (AJAX) or all screens (legacy)
    int s_start = 0, s_end = NUM_SCREENS;
    if (config_server.hasArg("save_screen")) {
        int save_only = config_server.arg("save_screen").toInt();
        if (save_only >= 0 && save_only < NUM_SCREENS) {
            s_start = save_only;
            s_end = save_only + 1;
        }
    }

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
            strncpy(screen_configs[s].icon_paths[g], iconValue.c_str(), 127);
            screen_configs[s].icon_paths[g][127] = '\0';
            // Save icon position
            String ipKey = "iconpos_" + String(s) + "_" + String(g);
            if (config_server.hasArg(ipKey)) {
                int ipos = config_server.arg(ipKey).toInt();
                if (ipos < 0) ipos = 0; if (ipos > 3) ipos = 3;
                screen_configs[s].icon_pos[g] = (uint8_t)ipos;
            }
            if (g == 0) {
                // Save per-screen background
                String bgKey = "bg_" + String(s);
                if (config_server.hasArg(bgKey)) {
                    String bgValue = config_server.arg(bgKey);
                    bgValue.replace("S://", "S:/");
                    while (bgValue.indexOf("//") != -1) bgValue.replace("//", "/");
                    strncpy(screen_configs[s].background_path, bgValue.c_str(), 127);
                    screen_configs[s].background_path[127] = '\0';
                }
                // Save show_bottom setting
                String sbKey = "showbottom_" + String(s);
                screen_configs[s].show_bottom = config_server.hasArg(sbKey) ? 1 : 0;

                // Save display type
                String dtKey = "dtype_" + String(s);
                if (config_server.hasArg(dtKey)) {
                    screen_configs[s].display_type = (uint8_t)config_server.arg(dtKey).toInt();
                }
                // Save Gauge+Number center config
                String gnPathKey = "gnum_path_" + String(s);
                if (config_server.hasArg(gnPathKey)) {
                    strncpy(screen_configs[s].gauge_num_center_path, config_server.arg(gnPathKey).c_str(), 127);
                    screen_configs[s].gauge_num_center_path[127] = '\0';
                }
                String gnFontKey = "gnum_font_" + String(s);
                if (config_server.hasArg(gnFontKey)) {
                    screen_configs[s].gauge_num_center_font_size = (uint8_t)config_server.arg(gnFontKey).toInt();
                }
                String gnColorKey = "gnum_color_" + String(s);
                if (config_server.hasArg(gnColorKey)) {
                    strncpy(screen_configs[s].gauge_num_center_font_color, config_server.arg(gnColorKey).c_str(), 7);
                    screen_configs[s].gauge_num_center_font_color[7] = '\0';
                }
            }
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
            for (int p = 0; p < 5; ++p) {
                String angleKey = "angle_" + String(s) + "_" + String(g) + "_" + String(p);
                String valueKey = "value_" + String(s) + "_" + String(g) + "_" + String(p);
                gauge_cal[s][g][p].angle = config_server.arg(angleKey).toInt();
                gauge_cal[s][g][p].value = config_server.arg(valueKey).toFloat();
            }
        }
    }

    // Sync gauge_cal → screen_configs.cal before SD/NVS writes.
    // The form values were written to gauge_cal above, but screen_configs.cal
    // still holds the old values. Without this copy the calibration angles
    // are silently lost when saving to SD.
    for (int s = s_start; s < s_end; ++s)
        for (int g = 0; g < 2; ++g)
            for (int p = 0; p < 5; ++p)
                screen_configs[s].cal[g][p] = gauge_cal[s][g][p];

    // Write per-screen binary configs to SD
    bool sd_available = true;
    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[SD SAVE] No SD card detected, attempting remount...");
        SD_MMC.end();
        if (!SD_MMC.begin("/sdcard", true)) {
            Serial.println("[SD SAVE] SD remount failed; skipping SD writes");
            sd_available = false;
        }
    }
    if (sd_available) {
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        for (int s2 = s_start; s2 < s_end; ++s2) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s2);
            File sf = SD_MMC.open(sdpath, FILE_WRITE);
            if (sf) {
                sf.write((const uint8_t *)&screen_configs[s2], sizeof(ScreenConfig));
                sf.flush();
                sf.close();
            }
        }
    }

    // Persist to NVS/Preferences
    save_preferences();

    // Refresh Signal K subscriptions
    refresh_signalk_subscriptions();

    // Hot-apply visuals
    apply_all_screen_visuals();
    skip_next_load_preferences = true;

    // Return JSON for AJAX caller
    config_server.sendHeader("Connection", "close");
    config_server.send(200, "application/json", "{\"ok\":true}");
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
    html += "</div>"; // root-actions
    html += "<div style='text-align:center;margin-top:18px;font-size:0.8em;color:#888;'>Firmware: " + String(FW_VERSION) + "</div>";
    html += "</div>"; // tab-content
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_scan_wifi() {
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

static String rssi_bar(int rssi) {
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
    html += "<div class='form-row'><label>SSID:</label>"
            "<input id='ssid' name='ssid' type='text' value='" + saved_ssid + "' style='width:40%'>"
            " <button type='button' id='scanBtn' onclick='scanWifi()' style='padding:6px 12px;cursor:pointer'>Scan</button>"
            "</div>";
    html += "<div id='scanResults' style='margin:0 0 10px 148px;display:none'></div>";
    html += "<div class='form-row'><label>Password:</label><input name='password' type='password' value='" + saved_password + "'></div>";
    html += "<div class='form-row'><label>SignalK Server:</label><input name='signalk_ip' type='text' value='" + saved_signalk_ip + "'></div>";
    html += "<div class='form-row'><label>SignalK Port:</label><input name='signalk_port' type='number' value='" + String(saved_signalk_port) + "'></div>";
    html += "<div class='form-row'><label>ESP32 Hostname:</label><input name='hostname' type='text' value='" + saved_hostname + "'></div>";
    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save & Reboot</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";

    // JavaScript for WiFi scanning
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


void handle_save_wifi() {
    if (config_server.method() == HTTP_POST) {
        saved_ssid = config_server.arg("ssid");
        saved_password = config_server.arg("password");
        saved_signalk_ip = config_server.arg("signalk_ip");
        saved_signalk_port = config_server.arg("signalk_port").toInt();
        saved_hostname = config_server.arg("hostname");
        save_preferences();
        Serial.println("[WiFi Config] SSID: " + saved_ssid);
        Serial.println("[WiFi Config] Password: " + saved_password);
        Serial.println("[WiFi Config] SignalK IP: " + saved_signalk_ip);
        Serial.print("[WiFi Config] SignalK Port: ");
        Serial.println(saved_signalk_port);
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
    // Brightness slider (0-100: above 50 = normal, below 50 = night mode)
    html += "<div class='form-row'><label>Brightness:</label><input id='brightness' name='brightness' type='range' min='0' max='100' value='" + String(brightness_slider_pos) + "' oninput=\"var v=this.value;document.getElementById('brightval').innerText=v;document.getElementById('modetext').innerText=v<50?' (Night Mode)':'';\"></div>";
    html += "<div class='form-row'><label>Current:</label><span id='brightval'>" + String(brightness_slider_pos) + "</span><span id='modetext'>" + String(brightness_slider_pos < 50 ? " (Night Mode)" : "") + "</span></div>";
    // Buzzer mode
    html += "<div class='form-row'><label>Buzzer Mode:</label><select name='buzzer_mode'>";
    html += "<option value='0'" + String(buzzer_mode==0?" selected":"") + ">Off</option>";
    html += "<option value='1'" + String(buzzer_mode==1?" selected":"") + ">Global</option>";
    html += "<option value='2'" + String(buzzer_mode==2?" selected":"") + ">Per-screen</option>";
    html += "</select></div>";
    // Buzzer cooldown (dropdown matching screen options)
    html += "<div class='form-row'><label>Buzzer Cooldown:</label><select name='buzzer_cooldown'>";
    html += "<option value='0'" + String(buzzer_cooldown_sec==0?" selected":"") + ">Constant</option>";
    html += "<option value='5'" + String(buzzer_cooldown_sec==5?" selected":"") + ">5s</option>";
    html += "<option value='10'" + String(buzzer_cooldown_sec==10?" selected":"") + ">10s</option>";
    html += "<option value='30'" + String(buzzer_cooldown_sec==30?" selected":"") + ">30s</option>";
    html += "<option value='60'" + String(buzzer_cooldown_sec==60?" selected":"") + ">60s</option>";
    html += "</select></div>";
    // Auto-scroll (dropdown matching screen options)
    html += "<div class='form-row'><label>Auto-scroll:</label><select name='auto_scroll'>";
    html += "<option value='0'" + String(auto_scroll_sec==0?" selected":"") + ">Off</option>";
    html += "<option value='5'" + String(auto_scroll_sec==5?" selected":"") + ">5s</option>";
    html += "<option value='10'" + String(auto_scroll_sec==10?" selected":"") + ">10s</option>";
    html += "<option value='30'" + String(auto_scroll_sec==30?" selected":"") + ">30s</option>";
    html += "<option value='60'" + String(auto_scroll_sec==60?" selected":"") + ">60s</option>";
    html += "</select></div>";
    // Unit system
    html += "<div class='form-row'><label>Units:</label><select name='unit_system'>";
    html += "<option value='0'" + String(unit_system==UNIT_METRIC?" selected":"") + ">Metric</option>";
    html += "<option value='1'" + String(unit_system==UNIT_IMPERIAL_US?" selected":"") + ">Imperial US</option>";
    html += "<option value='2'" + String(unit_system==UNIT_IMPERIAL_UK?" selected":"") + ">Imperial UK</option>";
    html += "<option value='3'" + String(unit_system==UNIT_NAUTICAL_METRIC?" selected":"") + ">Nautical Metric</option>";
    html += "<option value='4'" + String(unit_system==UNIT_NAUTICAL_IMP_US?" selected":"") + ">Nautical Imperial US</option>";
    html += "<option value='5'" + String(unit_system==UNIT_NAUTICAL_IMP_UK?" selected":"") + ">Nautical Imperial UK</option>";
    html += "</select></div>";
    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_save_device() {
    if (config_server.method() == HTTP_POST) {
        // Read and apply posted values
        int brightness = config_server.arg("brightness").toInt();
        int bm = config_server.arg("buzzer_mode").toInt();
        uint16_t bcd = (uint16_t)config_server.arg("buzzer_cooldown").toInt();
        uint16_t asc = (uint16_t)config_server.arg("auto_scroll").toInt();

        // Clamp slider position
        if (brightness < 0) brightness = 0;
        if (brightness > 100) brightness = 100;

        apply_brightness_from_slider((uint8_t)brightness);

        buzzer_mode = bm;
        buzzer_cooldown_sec = bcd;
        first_run_buzzer = true;
        Serial.printf("[DEVICE SAVE_POST] buzzer_mode=%d buzzer_cooldown_sec=%u first_run_buzzer=%d auto_scroll=%u brightness=%u\n",
                  buzzer_mode, buzzer_cooldown_sec, (int)first_run_buzzer, (unsigned)auto_scroll_sec, (unsigned)LCD_Backlight);

        auto_scroll_sec = asc;
        // Apply auto-scroll at runtime
        set_auto_scroll_interval(auto_scroll_sec);

        // Unit system
        uint16_t us = (uint16_t)config_server.arg("unit_system").toInt();
        if (us > 5) us = 3;
        unit_system = (UnitSystem)us;

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
    }
    if (WiFi.status() == WL_CONNECTED) {
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
        WiFi.softAP("ESP32-RoundDisplay", "12345678");
        Serial.print("AP IP: ");
        Serial.println(WiFi.softAPIP());
    }
    // Show fallback error screen if needed (after config load, before UI init)
    show_fallback_error_screen_if_needed();

    // Cache SD asset lists for AJAX page generation
    scan_sd_assets();

    // Register web UI routes and start server
    config_server.on("/", handle_root);
    config_server.on("/gauges", handle_gauges_page);
    config_server.on("/gauges/screen", handle_gauges_screen);
    config_server.on("/save-gauges", HTTP_POST, handle_save_gauges);
    config_server.on("/needles", handle_needles_page);
    config_server.on("/save-needles", HTTP_POST, handle_save_needles);
    // Assets manager page and upload/delete handlers
    config_server.on("/assets", handle_assets_page);
    config_server.on("/assets/upload", HTTP_POST, handle_assets_upload_post, handle_assets_upload);
    config_server.on("/assets/delete", HTTP_POST, handle_assets_delete);
    config_server.on("/network", handle_network_page);
    config_server.on("/scan-wifi", HTTP_GET, handle_scan_wifi);
    config_server.on("/save-wifi", HTTP_POST, handle_save_wifi);
    config_server.on("/device", handle_device_page);
    config_server.on("/save-device", HTTP_POST, handle_save_device);
    config_server.on("/test-gauge", HTTP_POST, handle_test_gauge);
    config_server.on("/toggle-test-mode", HTTP_POST, handle_toggle_test_mode);
    config_server.on("/set-screen", handle_set_screen);
    config_server.on("/nvs_test", HTTP_GET, handle_nvs_test);
    config_server.begin();
    Serial.println("[WebServer] Configuration web UI started on port 80");
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
    // Ensure assets dir exists
    if (!SD_MMC.exists("/assets")) {
        SD_MMC.mkdir("/assets");
    }
    String html = "<!DOCTYPE html><html><head>";
    html += STYLE;
    html += "<title>Assets Manager</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Assets Manager</h2>";
    // Upload form (styled)
    html += "<div class='assets-uploader'><form method='POST' action='/assets/upload' enctype='multipart/form-data' style='display:flex;gap:8px;align-items:center;'>";
    html += "<input type='file' name='file' accept='image/png,image/jpeg,image/bmp,image/gif'>";
    html += "<input type='submit' value='Upload' class='tab-btn'>";
    html += "</form></div>";

    html += "<h3>Files in /assets</h3>";
    html += "<table class='file-table'><tr><th>Name</th><th>Size</th><th>Actions</th></tr>";
    File root = SD_MMC.open("/assets");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            String fname = file.name();
            // show basename
            String bname = fname;
            if (bname.startsWith("/assets/")) bname = bname.substring(8);
            html += "<tr><td>" + bname + "</td><td class='file-size'>" + String(file.size()) + "</td>";
            html += "<td class='file-actions'><form method='POST' action='/assets/delete'><input type='hidden' name='file' value='" + bname + "'>";
            html += "<input type='submit' value='Delete' class='tab-btn' onclick='return confirm(\"Delete " + bname + "?\")'></form>";
            html += " <a href='S:/" + fname + "' target='_blank' class='tab-btn' style='padding:6px 10px;text-decoration:none;'>Download</a></td></tr>";
            file = root.openNextFile();
        }
    }
    html += "</table>";
    html += "<p style='text-align:center; margin-top:12px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
}

// Upload handler: called during multipart upload
static File assets_upload_file;
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
            Serial.printf("[ASSETS] Failed to open %s for writing\n", path.c_str());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (assets_upload_file) {
            assets_upload_file.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (assets_upload_file) {
            assets_upload_file.flush();
            assets_upload_file.close();
            Serial.printf("[ASSETS] Upload finished: %s (%u bytes)\n", upload.filename.c_str(), (unsigned)upload.totalSize);
        }
    }
}

// Final POST handler after upload completes (redirect back)
void handle_assets_upload_post() {
    scan_sd_assets(); // refresh cached asset lists after upload
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
    }
    scan_sd_assets(); // refresh cached asset lists after delete
    // redirect back
    config_server.sendHeader("Location", "/assets");
    config_server.send(303, "text/plain", "");
}
