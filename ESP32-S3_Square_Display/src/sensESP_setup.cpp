// ...existing code...

#include "gauge_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <vector>
#include <set>
#include "sensESP_setup.h"
#include "signalk_config.h"
#include "gauge_config.h"
#include "screen_config_c_api.h"
#include <FS.h>
#include <SPIFFS.h>
#include <SD_MMC.h>
#include <dirent.h>
#include <sys/stat.h>

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
    }
    if (all_default) {
        Serial.println("[ERROR] All screen configs are default/blank. Showing fallback error screen.");
        #ifdef LVGL_H
        lv_obj_t *scr = lv_scr_act();
        lv_obj_clean(scr);
        lv_obj_t *label = lv_label_create(scr);
        lv_label_set_text(label, "ERROR: No valid config loaded.\nCheck SD card or NVS.");
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        #endif
    }
}

// ...existing code...

#include "gauge_config.h"


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "sensESP_setup.h"
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

static const char *TAG_SETUP = "sensESP_setup";

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
        preferences.putUShort("brightness", (uint16_t)LCD_Backlight);
        // Save auto-scroll setting
        preferences.putUShort("auto_scroll", auto_scroll_sec);
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
                bst.close();
                Serial.println("[NVS REPAIR] Wrote /config/nvs_backup_settings.txt");
            } else {
                Serial.println("[NVS REPAIR] Failed to write settings backup to SD");
            }
            // Backup screen configs
            File bsf = SD_MMC.open("/config/nvs_backup_screens.bin", FILE_WRITE);
            if (bsf) {
                bsf.write((const uint8_t *)screen_configs, sizeof(ScreenConfig) * NUM_SCREENS);
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

    // Debug: print signalk_paths content before SD/NVS operations
    Serial.println("[DEBUG] signalk_paths before saving:");
    for (int i = 0; i < NUM_SCREENS * 2; ++i) {
        Serial.printf("[DEBUG] signalk_paths[%d] = '%s'\n", i, signalk_paths[i].c_str());
    }

    if (!any_nvs_ok) {
        Serial.println("[SD SAVE] NVS blob writes failed; saving screen configs to SD as fallback...");
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        for (int s = 0; s < NUM_SCREENS; ++s) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s);
            File f = SD_MMC.open(sdpath, FILE_WRITE);
            if (!f) { Serial.printf("[SD SAVE] Failed to open '%s'\n", sdpath); continue; }
            size_t written = f.write((const uint8_t *)&screen_configs[s], sizeof(ScreenConfig));
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
        preferences.end();
    } else {
        Serial.println("[DEBUG] preferences.begin(SETTINGS_NAMESPACE, true) failed for verification");
    }
    // Also print saved SSID/password from Preferences to verify
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {

    // Always write SignalK paths to SD as a fallback in case Preferences/NVS fails
    if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
    File spf = SD_MMC.open("/config/signalk_paths.txt", FILE_WRITE);
    if (spf) {
        for (int i = 0; i < NUM_SCREENS * 2; ++i) {
            spf.println(signalk_paths[i]);
        }
        spf.close();
        Serial.println("[SD SAVE] Wrote /config/signalk_paths.txt");
    } else {
        Serial.println("[SD SAVE] Failed to open /config/signalk_paths.txt for writing");
    }
        String vs = preferences.getString("ssid", "<missing>");
        String vp = preferences.getString("password", "<missing>");
        Serial.printf("[DEBUG] prefs saved SSID='%s' PASSWORD='%s'\n", vs.c_str(), vp.c_str());
        preferences.end();
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
        // Load device settings
        buzzer_mode = (int)preferences.getUShort("buzzer_mode", (uint16_t)buzzer_mode);
        buzzer_cooldown_sec = preferences.getUShort("buzzer_cooldown", buzzer_cooldown_sec);
        // Mark first run so buzzer logic can re-evaluate immediately
        first_run_buzzer = true;
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
    // If SignalK paths are empty in Preferences, try SD fallback file
    bool any_path_set = false;
    for (int i = 0; i < NUM_SCREENS * 2; ++i) if (signalk_paths[i].length() > 0) { any_path_set = true; break; }
    if (!any_path_set) {
        const char *spfpath = "/config/signalk_paths.txt";
        if (SD_MMC.exists(spfpath)) {
            File spf = SD_MMC.open(spfpath, FILE_READ);
            if (spf) {
                Serial.println("[SD LOAD] Loading SignalK paths from /config/signalk_paths.txt");
                int idx = 0;
                while (spf.available() && idx < NUM_SCREENS * 2) {
                    String line = spf.readStringUntil('\n');
                    line.trim();
                    signalk_paths[idx++] = line;
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
        // default to gauge display type
        screen_configs[s].display_type = DISPLAY_TYPE_GAUGE;
        // number display defaults (background uses bg_image field: empty/"Default" = default, bin path = file, "Custom Color" = color)
        strncpy(screen_configs[s].number_bg_color, "#000000", 7);
        screen_configs[s].number_font_size = 1;  // Medium (48pt)
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

    // If any screen config is still default, always restore from SD if available
    bool restored_from_sd = false;
    for (int s = 0; s < NUM_SCREENS; ++s) {
        bool is_default = true;
        for (int g = 0; g < 2 && is_default; ++g) {
            for (int p = 0; p < 5; ++p) {
                if (screen_configs[s].cal[g][p].angle != 0 || screen_configs[s].cal[g][p].value != 0.0f) {
                    is_default = false; break;
                }
            }
        }
        if (is_default) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s);
            if (SD_MMC.exists(sdpath)) {
                File f = SD_MMC.open(sdpath, FILE_READ);
                if (f) {
                    size_t got = f.read((uint8_t *)&screen_configs[s], sizeof(ScreenConfig));
                    Serial.printf("[SD LOAD] Read '%s' -> %u bytes (expected %u)\n", sdpath, (unsigned)got, (unsigned)sizeof(ScreenConfig));
                    f.close();
                    // Validate loaded config
                    bool valid = true;
                    for (int g = 0; g < 2 && valid; ++g) {
                        for (int p = 0; p < 5; ++p) {
                            if (screen_configs[s].cal[g][p].angle < 0 || screen_configs[s].cal[g][p].angle > 360) {
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
    // --- Scan SD card for available asset files and split into background and icon lists ---
    std::vector<String> iconFiles; // only .png files for icons
    std::vector<String> bgFiles;   // only .bin files for large backgrounds
    {
        File root = SD_MMC.open("/assets");
        if (root && root.isDirectory()) {
            File file = root.openNextFile();
            while (file) {
                String fname = file.name();
                Serial.printf("[ASSET SCAN] Found file: %s\n", fname.c_str());
                // Exclude macOS resource fork files (._ prefix)
                if (fname.startsWith("._")) {
                    file = root.openNextFile();
                    continue;
                }
                String lname = fname;
                lname.toLowerCase();
                // sanitize filenames that start with underscore
                if (lname.startsWith("_")) { file = root.openNextFile(); continue; }
                // Always add /assets/ prefix if not present
                String fullPath = fname;
                if (!fname.startsWith("/assets/")) {
                    fullPath = "/assets/" + fname;
                }
                if (lname.endsWith(".png")) {
                    iconFiles.push_back(String("S:/") + fullPath);
                } else if (lname.endsWith(".bin")) {
                    bgFiles.push_back(String("S:/") + fullPath);
                }
                file = root.openNextFile();
            }
        } else {
            // POSIX fallback: scan /sdcard/assets
            DIR *d = opendir("/sdcard/assets");
            if (d) {
                struct dirent *entry;
                while ((entry = readdir(d)) != NULL) {
                    const char *fname = entry->d_name;
                    if (!fname) continue;
                    if (fname[0] == '.') continue;
                    String sname = String(fname);
                    String lname = sname;
                    lname.toLowerCase();
                    if (lname.startsWith("_")) continue;
                    String fullPath = "/assets/" + sname;
                    if (lname.endsWith(".png")) iconFiles.push_back(String("S:/") + fullPath);
                    else if (lname.endsWith(".bin")) bgFiles.push_back(String("S:/") + fullPath);
                }
                closedir(d);
            }
        }
    }
    // --- End SD scan ---
    // Reload config from storage unless a save just occurred — in that case
    // prefer the in-memory `screen_configs` so the UI shows the recently-saved values.
    if (!skip_next_load_preferences) {
        load_preferences();
    } else {
        // consume the skip flag and keep current in-memory configs
        skip_next_load_preferences = false;
    }
    Serial.println("[DEBUG] handle_gauges_page() - gauge_cal values sent to HTML:");
    for (int s = 0; s < NUM_SCREENS; ++s) {
        for (int g = 0; g < 2; ++g) {
            for (int p = 0; p < 5; ++p) {
                Serial.printf("[DEBUG] gauge_cal[%d][%d][%d]: angle=%d value=%.2f\n", s, g, p, gauge_cal[s][g][p].angle, gauge_cal[s][g][p].value);
            }
        }
    }
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += STYLE;
    html += "<title>Gauge Calibration</title></head><body><div class='container'>";
    extern bool test_mode;
    html += "<h2>Gauge Calibration</h2>";
    html += "<form method='POST' action='/toggle-test-mode' style='margin-bottom:16px;text-align:center;'>";
    // hidden active tab for toggle button to preserve current tab
    html += "<input type='hidden' name='active_tab' id='active_tab_toggle' value='0'>";
    html += "<input type='hidden' name='toggle' value='1'>";
    html += "<button type='submit' style='padding:8px 16px;font-size:1em;'>";
    html += (test_mode ? "Disable Setup Mode" : "Enable Setup Mode");
    html += "</button> ";
    html += "<span style='font-weight:bold;color:";
    html += (test_mode ? "#388e3c;'>SETUP MODE ON" : "#b71c1c;'>SETUP MODE OFF");
    html += "</span></form>";
        // --- Scan SD card for available image files (PNG, BIN, JPG, BMP, GIF) ---
        // ...existing code...
        // --- End SD scan ---
    // Calibration form start
    html += "<form id='calibrationForm' method='POST' action='/save-gauges' accept-charset='utf-8'>";
    // Hidden field to remember which tab the user had active when submitting
    html += "<input type='hidden' name='active_tab' id='active_tab' value='0'>";
    // Tab bar
    html += "<div style='margin-bottom:16px; text-align:center;'>";
        for (int s = 0; s < NUM_SCREENS; ++s) {
            // When clicked: switch the web tab AND request the device to show that screen
            int screen_one_based = s + 1;
            html += "<button type='button' class='tab-btn' id='tabbtn_" + String(s) + "' onclick='(function(){ showScreenTab(" + String(s) + "); fetch(\"/set-screen?screen=" + String(screen_one_based) + "\", {method:\"GET\"}).catch(function(){ }); })()' style='margin:0 4px; padding:8px 16px; font-size:1em;'>Screen " + String(screen_one_based) + "</button>";
        }
    html += "</div>";
    // Tab content
    for (int s = 0; s < NUM_SCREENS; ++s) {
        html += "<div class='tab-content' id='tabcontent_" + String(s) + "' style='display:" + (s==0?"block":"none") + ";'>";
        html += "<h3>Screen " + String(s+1) + "</h3>";
        
        // Display Type dropdown (new)
        html += "<div style='margin-bottom:16px;'><label>Display Type: <select name='displaytype_" + String(s) + "' id='displaytype_" + String(s) + "' onchange='toggleGaugeConfig(" + String(s) + ")'>";
        html += "<option value='0'";
        if (screen_configs[s].display_type == 0) html += " selected";
        html += ">Gauge</option>";
        html += "<option value='1'";
        if (screen_configs[s].display_type == 1) html += " selected";
        html += ">Number</option>";
        html += "<option value='2'";
        if (screen_configs[s].display_type == 2) html += " selected";
        html += ">Dual</option>";
        html += "<option value='3'";
        if (screen_configs[s].display_type == 3) html += " selected";
        html += ">Quad</option>";
        html += "<option value='4'";
        if (screen_configs[s].display_type == 4) html += " selected";
        html += ">Gauge + Number</option>";
        html += "<option value='5'";
        if (screen_configs[s].display_type == 5) html += " selected";
        html += ">Graph</option>";
        html += "</select></label></div>";
        
        // Background selection (per-screen)
        String savedBg = String(screen_configs[s].background_path);
        String savedBgNorm = savedBg;
        savedBgNorm.toLowerCase();
        savedBgNorm.replace("S://", "S:/");
        while (savedBgNorm.indexOf("//") != -1) savedBgNorm.replace("//", "/");
        html += "<div style='margin-bottom:8px;'><label>Background: <select name='bg_" + String(s) + "' id='bg_image_" + String(s) + "' onchange='toggleBgImageColor(" + String(s) + ")'>";
        html += "<option value=''";
        if (savedBg.length() == 0) html += " selected='selected'";
        html += ">Default</option>";
        for (const auto& b : bgFiles) {
            String iconNorm = b;
            iconNorm.toLowerCase();
            iconNorm.replace("S://", "S:/");
            while (iconNorm.indexOf("//") != -1) iconNorm.replace("//", "/");
            html += "<option value='" + b + "'";
            if (iconNorm == savedBgNorm && savedBg.length() > 0) html += " selected='selected'";
            html += ">" + b + "</option>";
        }
        // Add Custom Color option for number displays
        html += "<option value='Custom Color'";
        if (savedBg == "Custom Color") html += " selected='selected'";
        html += ">Custom Color</option>";
        html += "</select></label></div>";
        
        // Number display configuration container (shown when display_type is Number)
        html += "<div id='numberconfig_" + String(s) + "' style='display:" + String(screen_configs[s].display_type == 1 ? "block" : "none") + ";'>";
        html += "<h4>Number Display Settings</h4>";
        
        // SignalK Path for number display
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='number_path_" + String(s) + "' type='text' value='" + String(screen_configs[s].number_path) + "' style='width:80%'></label></div>";
        
        // Background color (shown when Custom Color is selected in bg_image dropdown)
        bool isCustomColor = (String(screen_configs[s].background_path) == "Custom Color");
        html += "<div id='number_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColor ? "block" : "none") + ";'>";
        html += "<label>Background Color: <input name='number_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
        
        // Font size
        html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='number_font_size_" + String(s) + "'>";
        html += "<option value='0'";
        if (screen_configs[s].number_font_size == 0) html += " selected";
        html += ">Small (48pt)</option>";
        html += "<option value='1'";
        if (screen_configs[s].number_font_size == 1) html += " selected";
        html += ">Medium (72pt)</option>";
        html += "<option value='2'";
        if (screen_configs[s].number_font_size == 2) html += " selected";
        html += ">Large (96pt)</option>";
        html += "<option value='3'";
        if (screen_configs[s].number_font_size == 3) html += " selected";
        html += ">X-Large (120pt)</option>";
        html += "<option value='4'";
        if (screen_configs[s].number_font_size == 4) html += " selected";
        html += ">XX-Large (144pt)</option>";
        html += "</select></label></div>";
        
        // Font color
        html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='number_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_font_color[0] ? screen_configs[s].number_font_color : "#FFFFFF") + "'></label></div>";
        
        html += "</div>"; // End number display config
        
        // Dual display configuration container (shown when display_type is Dual)
        html += "<div id='dualconfig_" + String(s) + "' style='display:" + String(screen_configs[s].display_type == 2 ? "block" : "none") + ";'>";
        html += "<h4>Dual Display Settings</h4>";
        
        // Background color (shown when Custom Color is selected in bg_image dropdown)
        html += "<div id='dual_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColor ? "block" : "none") + ";'>";
        html += "<label>Background Color: <input name='dual_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
        
        // Top display settings
        html += "<h5>Top Display</h5>";
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='dual_top_path_" + String(s) + "' type='text' value='" + String(screen_configs[s].dual_top_path) + "' style='width:80%'></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='dual_top_font_size_" + String(s) + "'>";
        html += "<option value='0'";
        if (screen_configs[s].dual_top_font_size == 0) html += " selected";
        html += ">Small (48pt)</option>";
        html += "<option value='1'";
        if (screen_configs[s].dual_top_font_size == 1) html += " selected";
        html += ">Medium (72pt)</option>";
        html += "<option value='2'";
        if (screen_configs[s].dual_top_font_size == 2) html += " selected";
        html += ">Large (96pt)</option>";
        html += "<option value='3'";
        if (screen_configs[s].dual_top_font_size == 3) html += " selected";
        html += ">X-Large (120pt)</option>";
        html += "<option value='4'";
        if (screen_configs[s].dual_top_font_size == 4) html += " selected";
        html += ">XX-Large (144pt)</option>";
        html += "</select></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='dual_top_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].dual_top_font_color[0] ? screen_configs[s].dual_top_font_color : "#FFFFFF") + "'></label></div>";
        
        // Bottom display settings
        html += "<h5>Bottom Display</h5>";
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='dual_bottom_path_" + String(s) + "' type='text' value='" + String(screen_configs[s].dual_bottom_path) + "' style='width:80%'></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='dual_bottom_font_size_" + String(s) + "'>";
        html += "<option value='0'";
        if (screen_configs[s].dual_bottom_font_size == 0) html += " selected";
        html += ">Small (48pt)</option>";
        html += "<option value='1'";
        if (screen_configs[s].dual_bottom_font_size == 1) html += " selected";
        html += ">Medium (72pt)</option>";
        html += "<option value='2'";
        if (screen_configs[s].dual_bottom_font_size == 2) html += " selected";
        html += ">Large (96pt)</option>";
        html += "<option value='3'";
        if (screen_configs[s].dual_bottom_font_size == 3) html += " selected";
        html += ">X-Large (120pt)</option>";
        html += "<option value='4'";
        if (screen_configs[s].dual_bottom_font_size == 4) html += " selected";
        html += ">XX-Large (144pt)</option>";
        html += "</select></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='dual_bottom_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].dual_bottom_font_color[0] ? screen_configs[s].dual_bottom_font_color : "#FFFFFF") + "'></label></div>";
        
        html += "</div>"; // End dual display config
        
        // Quad display configuration (hidden when display_type is not Quad)
        html += "<div id='quadconfig_" + String(s) + "' style='display:" + String(screen_configs[s].display_type == 3 ? "block" : "none") + ";'>";
        html += "<h4>Quad Display Settings</h4>";
        
        // Background color
        html += "<div style='margin-bottom:8px;'><label>Background Color: <input name='quad_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
        
        // Helper function for quad quadrant HTML (we'll define it inline)
        auto addQuadrantHTML = [&](const char* name, const char* label, char* path, uint8_t size, char* color) {
            html += "<h5>" + String(label) + "</h5>";
            html += "<div style='margin-bottom:4px;'><label>SignalK Path: <input name='quad_" + String(name) + "_path_" + String(s) + "' type='text' value='" + String(path) + "' style='width:80%'></label></div>";
            html += "<div style='margin-bottom:4px;'><label>Font Size: <select name='quad_" + String(name) + "_font_size_" + String(s) + "'>";
            for (int fs = 0; fs < 3; fs++) {  // Only show Small, Medium, Large (0-2)
                html += "<option value='" + String(fs) + "'";
                if (size == fs) html += " selected";
                html += ">";
                if (fs == 0) html += "Small (48pt)";
                else if (fs == 1) html += "Medium (72pt)";
                else if (fs == 2) html += "Large (96pt)";
                html += "</option>";
            }
            html += "</select></label></div>";
            html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='quad_" + String(name) + "_font_color_" + String(s) + "' type='color' value='" + String(color[0] ? color : "#FFFFFF") + "'></label></div>";
        };
        
        addQuadrantHTML("tl", "Top-Left", screen_configs[s].quad_tl_path, screen_configs[s].quad_tl_font_size, screen_configs[s].quad_tl_font_color);
        addQuadrantHTML("tr", "Top-Right", screen_configs[s].quad_tr_path, screen_configs[s].quad_tr_font_size, screen_configs[s].quad_tr_font_color);
        addQuadrantHTML("bl", "Bottom-Left", screen_configs[s].quad_bl_path, screen_configs[s].quad_bl_font_size, screen_configs[s].quad_bl_font_color);
        addQuadrantHTML("br", "Bottom-Right", screen_configs[s].quad_br_path, screen_configs[s].quad_br_font_size, screen_configs[s].quad_br_font_color);
        
        html += "</div>"; // End quad display config
        
        // Gauge configuration container (hidden when display_type is Number)
        html += "<div id='gaugeconfig_" + String(s) + "' style='display:" + String(screen_configs[s].display_type == 0 ? "block" : "none") + ";'>";
        
        for (int g = 0; g < 2; ++g) {
            // When rendering UI: allow user to hide bottom gauge per-screen
            if (g == 0) {
                html += "<div style='margin-bottom:8px;'><label>Show Bottom Gauge: <input type='checkbox' name='showbottom_" + String(s) + "'";
                if (screen_configs[s].show_bottom) html += " checked";
                html += "></label></div>";
            }
            int idx = s * 2 + g;
            // If bottom gauge is disabled, skip rendering its configuration
            if (g == 1 && !screen_configs[s].show_bottom) {
                html += "<div style='margin-bottom:8px;'><em>Bottom gauge disabled for this screen.</em></div>";
                continue;
            }
            html += "<b>" + String(g == 0 ? "Top Gauge" : "Bottom Gauge") + "</b>";
            // SignalK Path: show immediately above the icon options (per-gauge)
            html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='skpath_" + String(s) + "_" + String(g) + "' type='text' value='" + signalk_paths[idx] + "' style='width:80%'></label></div>";

            // Calibration points table (moved to be under SignalK Path)
            html += "<table class='table'><tr><th>Point</th><th>Angle</th><th>Value</th><th>Test</th></tr>";
            for (int p = 0; p < 5; ++p) {
                html += "<tr><td>" + String(p+1) + "</td>";
                html += "<td><input name='" + String("angle_") + s + "_" + g + "_" + p + "' type='number' value='" + String(gauge_cal[s][g][p].angle) + "'></td>";
                html += "<td><input name='" + String("value_") + s + "_" + g + "_" + p + "' type='number' step='any' value='" + String(gauge_cal[s][g][p].value) + "'></td>";
                html += "<td><button type='button' onclick='testGaugePoint(" + String(s) + "," + String(g) + "," + String(p) + ")' ";
                html += (test_mode ? "" : "disabled ");
                html += "style='padding:4px 8px;font-size:0.9em;background-color:";
                html += (test_mode ? "#4a90e2" : "#cccccc");
                html += ";color:#ffffff;border:1px solid #2d5a8f;border-radius:4px;cursor:";
                html += (test_mode ? "pointer" : "not-allowed");
                html += ";'>Test</button></td></tr>";
            }
            html += "</table>";

            // Icon controls (grouped visually)
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
            for (const auto& icon : iconFiles) {
                String iconNorm = icon;
                iconNorm.toLowerCase();
                iconNorm.replace("S://", "S:/");
                while (iconNorm.indexOf("//") != -1) iconNorm.replace("//", "/");
                html += "<option value='" + icon + "'";
                if (iconNorm == savedIconNorm && savedIcon.length() > 0) html += " selected='selected'";
                html += ">" + icon + "</option>";
            }
            html += "</select></label></div>";
            // Icon position selector (per-gauge)
            int curPos = screen_configs[s].icon_pos[g];
            html += "<div style='margin-bottom:8px;'><label>Icon Position: <select name='iconpos_" + String(s) + "_" + String(g) + "'>";
            struct { int v; const char *n; } posopts[] = { {0,"Top"}, {1,"Right"}, {2,"Bottom"}, {3,"Left"} };
            for (int _po = 0; _po < 4; ++_po) {
                html += "<option value='" + String(posopts[_po].v) + "'";
                if (curPos == posopts[_po].v) html += " selected='selected'";
                html += ">" + String(posopts[_po].n) + "</option>";
            }
            html += "</select></label></div>";
            // close icon-row but keep icon-section open so min/max controls are grouped with icons
            html += "</div>"; // close icon-row

            // Zone min/max/color/transparent/buzzer controls — include inside icon-section
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
        }
        html += "</div>"; // close gaugeconfig div
        
        // Gauge + Number configuration (display_type == 4)
        html += "<div id='gaugenumconfig_" + String(s) + "' style='display:" + String(screen_configs[s].display_type == 4 ? "block" : "none") + ";'>";
        html += "<h4>Gauge + Number Display Settings</h4>";
        
        // Top gauge configuration (g=0 only)
        html += "<b>Top Gauge</b>";
        int idx = s * 2 + 0; // Always use top gauge (g=0)
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='skpath_" + String(s) + "_0' type='text' value='" + signalk_paths[idx] + "' style='width:80%'></label></div>";
        
        // Calibration points table
        html += "<table class='table'><tr><th>Point</th><th>Angle</th><th>Value</th><th>Test</th></tr>";
        for (int p = 0; p < 5; ++p) {
            html += "<tr><td>" + String(p+1) + "</td>";
            html += "<td><input name='" + String("angle_") + s + "_0_" + p + "' type='number' value='" + String(gauge_cal[s][0][p].angle) + "'></td>";
            html += "<td><input name='" + String("value_") + s + "_0_" + p + "' type='number' step='any' value='" + String(gauge_cal[s][0][p].value) + "'></td>";
            html += "<td><button type='button' onclick='testGaugePoint(" + String(s) + ",0," + String(p) + ")' ";
            html += (test_mode ? "" : "disabled ");
            html += "style='padding:4px 8px;font-size:0.9em;background-color:";
            html += (test_mode ? "#4a90e2" : "#cccccc");
            html += ";color:#ffffff;border:1px solid #2d5a8f;border-radius:4px;cursor:";
            html += (test_mode ? "pointer" : "not-allowed");
            html += ";'>Test</button></td></tr>";
        }
        html += "</table>";
        
        // Icon controls for top gauge
        String savedIcon = String(screen_configs[s].icon_paths[0]);
        String savedIconNorm = savedIcon;
        savedIconNorm.toLowerCase();
        savedIconNorm.replace("S://", "S:/");
        while (savedIconNorm.indexOf("//") != -1) savedIconNorm.replace("//", "/");
        html += "<div class='icon-section'><div class='icon-row'>";
        html += "<div style='margin-bottom:8px;'><label>Icon: <select name='icon_" + String(s) + "_0'>";
        html += "<option value=''";
        if (savedIcon.length() == 0) html += " selected='selected'";
        html += ">None</option>";
        for (const auto& icon : iconFiles) {
            String iconNorm = icon;
            iconNorm.toLowerCase();
            iconNorm.replace("S://", "S:/");
            while (iconNorm.indexOf("//") != -1) iconNorm.replace("//", "/");
            html += "<option value='" + icon + "'";
            if (iconNorm == savedIconNorm && savedIcon.length() > 0) html += " selected='selected'";
            html += ">" + icon + "</option>";
        }
        html += "</select></label></div>";
        
        // Icon position for top gauge
        int curPos = screen_configs[s].icon_pos[0];
        html += "<div style='margin-bottom:8px;'><label>Icon Position: <select name='iconpos_" + String(s) + "_0'>";
        struct { int v; const char *n; } posopts[] = { {0,"Top"}, {1,"Right"}, {2,"Bottom"}, {3,"Left"} };
        for (int _po = 0; _po < 4; ++_po) {
            html += "<option value='" + String(posopts[_po].v) + "'";
            if (curPos == posopts[_po].v) html += " selected='selected'";
            html += ">" + String(posopts[_po].n) + "</option>";
        }
        html += "</select></label></div>";
        html += "</div>"; // close icon-row
        
        // Zone controls for top gauge
        html += "<div class='zone-row'>";
        for (int i = 1; i <= 4; ++i) {
            float minVal = screen_configs[s].min[0][i];
            float maxVal = screen_configs[s].max[0][i];
            String colorVal = String(screen_configs[s].color[0][i]);
            bool transVal = screen_configs[s].transparent[0][i] != 0;
            bool bzrVal = screen_configs[s].buzzer[0][i] != 0;
            html += "<div class='zone-item'><label>Min " + String(i) + ": <input name='mnv" + String(s) + "0" + String(i) + "' type='number' step='any' value='" + String(minVal) + "' style='width:100px'></label></div>";
            html += "<div class='zone-item'><label>Max " + String(i) + ": <input name='mxv" + String(s) + "0" + String(i) + "' type='number' step='any' value='" + String(maxVal) + "' style='width:100px'></label></div>";
            html += "<div class='zone-item'><label>Color: <input class='color-input' name='clr" + String(s) + "0" + String(i) + "' type='color' value='" + colorVal + "'></label></div>";
            html += "<div class='zone-item small'><label>Transparent <input name='trn" + String(s) + "0" + String(i) + "' type='checkbox'";
            if (transVal) html += " checked";
            html += "></label></div>";
            html += "<div class='zone-item small'><label>Buzzer <input name='bzr" + String(s) + "0" + String(i) + "' type='checkbox'";
            if (bzrVal) html += " checked";
            html += "></label></div>";
        }
        html += "</div>";
        html += "</div>"; // close icon-section
        
        // Center number display configuration
        html += "<h5 style='margin-top:16px;'>Center Number Display</h5>";
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='gauge_num_center_path_" + String(s) + "' type='text' value='" + String(screen_configs[s].gauge_num_center_path) + "' style='width:80%'></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Font Size: <select name='gauge_num_center_font_size_" + String(s) + "'>";
        html += "<option value='0'";
        if (screen_configs[s].gauge_num_center_font_size == 0) html += " selected";
        html += ">Small (48pt)</option>";
        html += "<option value='1'";
        if (screen_configs[s].gauge_num_center_font_size == 1) html += " selected";
        html += ">Medium (72pt)</option>";
        html += "<option value='2'";
        if (screen_configs[s].gauge_num_center_font_size == 2) html += " selected";
        html += ">Large (96pt)</option>";
        html += "<option value='3'";
        if (screen_configs[s].gauge_num_center_font_size == 3) html += " selected";
        html += ">X-Large (120pt)</option>";
        html += "<option value='4'";
        if (screen_configs[s].gauge_num_center_font_size == 4) html += " selected";
        html += ">XX-Large (144pt)</option>";
        html += "</select></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Font Color: <input name='gauge_num_center_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].gauge_num_center_font_color[0] ? screen_configs[s].gauge_num_center_font_color : "#FFFFFF") + "'></label></div>";
        
        html += "</div>"; // close gaugenumconfig div
        
        // Graph configuration (display_type == 5)
        html += "<div id='graphconfig_" + String(s) + "' style='display:" + String(screen_configs[s].display_type == 5 ? "block" : "none") + ";'>";
        html += "<h4>Graph Display Settings</h4>";
        
        // SignalK Path for graph display (reuses number_path field)
        html += "<div style='margin-bottom:8px;'><label>SignalK Path: <input name='number_path_" + String(s) + "' type='text' value='" + String(screen_configs[s].number_path) + "' style='width:80%'></label></div>";
        
        // Chart Type selection
        html += "<div style='margin-bottom:8px;'><label>Chart Type: <select name='graph_chart_type_" + String(s) + "'>";
        html += "<option value='0'";
        if (screen_configs[s].graph_chart_type == 0) html += " selected";
        html += ">Line Chart</option>";
        html += "<option value='1'";
        if (screen_configs[s].graph_chart_type == 1) html += " selected";
        html += ">Bar Chart</option>";
        html += "<option value='2'";
        if (screen_configs[s].graph_chart_type == 2) html += " selected";
        html += ">Scatter Plot</option>";
        html += "</select></label></div>";
        
        // Time Range selection
        html += "<div style='margin-bottom:8px;'><label>Time Range: <select name='graph_time_range_" + String(s) + "'>";
        html += "<option value='0'";
        if (screen_configs[s].graph_time_range == 0) html += " selected";
        html += ">10 seconds</option>";
        html += "<option value='1'";
        if (screen_configs[s].graph_time_range == 1) html += " selected";
        html += ">30 seconds</option>";
        html += "<option value='2'";
        if (screen_configs[s].graph_time_range == 2) html += " selected";
        html += ">1 minute</option>";
        html += "<option value='3'";
        if (screen_configs[s].graph_time_range == 3) html += " selected";
        html += ">5 minutes</option>";
        html += "<option value='4'";
        if (screen_configs[s].graph_time_range == 4) html += " selected";
        html += ">10 minutes</option>";
        html += "<option value='5'";
        if (screen_configs[s].graph_time_range == 5) html += " selected";
        html += ">30 minutes</option>";
        html += "</select></label></div>";
        
        // Font color for graph (labels, axes, line color)
        html += "<div style='margin-bottom:8px;'><label>Series 1 Color: <input name='number_font_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_font_color[0] ? screen_configs[s].number_font_color : "#00FF00") + "'></label></div>";
        
        // Second series configuration
        html += "<h5 style='margin-top:16px;'>Second Data Series (Optional)</h5>";
        html += "<div style='margin-bottom:8px;'><label>SignalK Path 2: <input name='graph_path_2_" + String(s) + "' type='text' value='" + String(screen_configs[s].graph_path_2) + "' style='width:80%'></label></div>";
        html += "<div style='margin-bottom:8px;'><label>Series 2 Color: <input name='graph_color_2_" + String(s) + "' type='color' value='" + String(screen_configs[s].graph_color_2[0] ? screen_configs[s].graph_color_2 : "#FF0000") + "'></label></div>";
        
        // Background color (shown when Custom Color is selected in bg_image dropdown)
        bool isCustomColorGraph = (String(screen_configs[s].background_path) == "Custom Color");
        html += "<div id='graph_bg_color_div_" + String(s) + "' style='margin-bottom:8px;display:" + String(isCustomColorGraph ? "block" : "none") + ";'>";
        html += "<label>Background Color: <input name='number_bg_color_" + String(s) + "' type='color' value='" + String(screen_configs[s].number_bg_color[0] ? screen_configs[s].number_bg_color : "#000000") + "'></label></div>";
        
        html += "</div>"; // close graphconfig div
        
        html += "</div>"; // close tab content
    }
    // Tab JS and Apply button (ensure inside form)
    html += "<div style='text-align:center; margin-top:16px;'><input type='submit' name='apply' value='Apply (no reboot)' style='padding:10px 24px; font-size:1.1em;'></div>";
    html += "</form>";
    // Now add the test buttons outside the main form
    for (int s = 0; s < NUM_SCREENS; ++s) {
        for (int g = 0; g < 2; ++g) {
            for (int p = 0; p < 5; ++p) {
                html += "<form style='display:none;' id='testform_" + String(s) + "_" + String(g) + "_" + String(p) + "' method='POST' action='/test-gauge'>";
                html += "<input type='hidden' name='screen' value='" + String(s) + "'>";
                html += "<input type='hidden' name='gauge' value='" + String(g) + "'>";
                html += "<input type='hidden' name='point' value='" + String(p) + "'>";
                html += "<input type='hidden' name='angle' id='testangle_" + String(s) + "_" + String(g) + "_" + String(p) + "' value=''>";
                html += "</form>";
            }
        }
    }
    html += "<script>function testGaugePoint(screen,gauge,point){\n";
    html += "  var angleInput = document.querySelector('input[name=\"angle_'+screen+'_'+gauge+'_'+point+'\"]');\n";
    html += "  var testAngle = document.getElementById('testangle_'+screen+'_'+gauge+'_'+point);\n";
    html += "  if(angleInput && testAngle){ testAngle.value = angleInput.value; }\n";
    html += "  document.getElementById('testform_'+screen+'_'+gauge+'_'+point).submit();\n";
    html += "}\n";
    html += "function showScreenTab(idx){\n";
    html += "  for(var s=0;s<" + String(NUM_SCREENS) + ";++s){\n";
    html += "    var el = document.getElementById('tabcontent_'+s); if(el) el.style.display=(s==idx?'block':'none');\n";
    html += "    var btn=document.getElementById('tabbtn_'+s);\n";
    html += "    if(btn)btn.style.background=(s==idx?'#e3eaf6':'#f4f6fa');\n";
    html += "  }\n";
    html += "  var hidden = document.getElementById('active_tab'); if(hidden) hidden.value = idx;\n";
    html += "  var hidden2 = document.getElementById('active_tab_toggle'); if(hidden2) hidden2.value = idx;\n";
    html += "  try{ history.replaceState && history.replaceState(null,null,'#tab'+idx); }catch(e){}\n";
    html += "}\n";
    html += "function toggleGaugeConfig(screen){\n";
    html += "  var sel = document.getElementById('displaytype_'+screen);\n";
    html += "  var gaugeDiv = document.getElementById('gaugeconfig_'+screen);\n";
    html += "  var numberDiv = document.getElementById('numberconfig_'+screen);\n";
    html += "  var dualDiv = document.getElementById('dualconfig_'+screen);\n";
    html += "  var quadDiv = document.getElementById('quadconfig_'+screen);\n";
    html += "  var gaugeNumDiv = document.getElementById('gaugenumconfig_'+screen);\n";
    html += "  var graphDiv = document.getElementById('graphconfig_'+screen);\n";
    html += "  console.log('toggleGaugeConfig('+screen+'): sel.value='+sel.value+', gaugeNumDiv exists:', !!gaugeNumDiv, ', graphDiv exists:', !!graphDiv);\n";
    html += "  function disableInputs(div, disable) {\n";
    html += "    if(!div) return;\n";
    html += "    var inputs = div.querySelectorAll('input, select, textarea');\n";
    html += "    for(var i=0; i<inputs.length; i++) inputs[i].disabled = disable;\n";
    html += "    // Don't touch buttons - their disabled state is managed by test mode toggle\n";
    html += "  }\n";
    html += "  if(sel && gaugeDiv && numberDiv && dualDiv && quadDiv && gaugeNumDiv && graphDiv){\n";
    html += "    if(sel.value === '0'){\n";
    html += "      gaugeDiv.style.display = 'block'; disableInputs(gaugeDiv, false);\n";
    html += "      numberDiv.style.display = 'none'; disableInputs(numberDiv, true);\n";
    html += "      dualDiv.style.display = 'none'; disableInputs(dualDiv, true);\n";
    html += "      quadDiv.style.display = 'none'; disableInputs(quadDiv, true);\n";
    html += "      gaugeNumDiv.style.display = 'none'; disableInputs(gaugeNumDiv, true);\n";
    html += "      graphDiv.style.display = 'none'; disableInputs(graphDiv, true);\n";
    html += "    } else if(sel.value === '1'){\n";
    html += "      gaugeDiv.style.display = 'none'; disableInputs(gaugeDiv, true);\n";
    html += "      numberDiv.style.display = 'block'; disableInputs(numberDiv, false);\n";
    html += "      dualDiv.style.display = 'none'; disableInputs(dualDiv, true);\n";
    html += "      quadDiv.style.display = 'none'; disableInputs(quadDiv, true);\n";
    html += "      gaugeNumDiv.style.display = 'none'; disableInputs(gaugeNumDiv, true);\n";
    html += "      graphDiv.style.display = 'none'; disableInputs(graphDiv, true);\n";
    html += "    } else if(sel.value === '2'){\n";
    html += "      gaugeDiv.style.display = 'none'; disableInputs(gaugeDiv, true);\n";
    html += "      numberDiv.style.display = 'none'; disableInputs(numberDiv, true);\n";
    html += "      dualDiv.style.display = 'block'; disableInputs(dualDiv, false);\n";
    html += "      quadDiv.style.display = 'none'; disableInputs(quadDiv, true);\n";
    html += "      gaugeNumDiv.style.display = 'none'; disableInputs(gaugeNumDiv, true);\n";
    html += "      graphDiv.style.display = 'none'; disableInputs(graphDiv, true);\n";
    html += "    } else if(sel.value === '3'){\n";
    html += "      gaugeDiv.style.display = 'none'; disableInputs(gaugeDiv, true);\n";
    html += "      numberDiv.style.display = 'none'; disableInputs(numberDiv, true);\n";
    html += "      dualDiv.style.display = 'none'; disableInputs(dualDiv, true);\n";
    html += "      quadDiv.style.display = 'block'; disableInputs(quadDiv, false);\n";
    html += "      gaugeNumDiv.style.display = 'none'; disableInputs(gaugeNumDiv, true);\n";
    html += "      graphDiv.style.display = 'none'; disableInputs(graphDiv, true);\n";
    html += "    } else if(sel.value === '4'){\n";
    html += "      gaugeDiv.style.display = 'none'; disableInputs(gaugeDiv, true);\n";
    html += "      numberDiv.style.display = 'none'; disableInputs(numberDiv, true);\n";
    html += "      dualDiv.style.display = 'none'; disableInputs(dualDiv, true);\n";
    html += "      quadDiv.style.display = 'none'; disableInputs(quadDiv, true);\n";
    html += "      gaugeNumDiv.style.display = 'block'; disableInputs(gaugeNumDiv, false);\n";
    html += "      graphDiv.style.display = 'none'; disableInputs(graphDiv, true);\n";
    html += "      console.log('Set gaugeNumDiv display to block for screen '+screen);\n";
    html += "    } else if(sel.value === '5'){\n";
    html += "      gaugeDiv.style.display = 'none'; disableInputs(gaugeDiv, true);\n";
    html += "      numberDiv.style.display = 'none'; disableInputs(numberDiv, true);\n";
    html += "      dualDiv.style.display = 'none'; disableInputs(dualDiv, true);\n";
    html += "      quadDiv.style.display = 'none'; disableInputs(quadDiv, true);\n";
    html += "      gaugeNumDiv.style.display = 'none'; disableInputs(gaugeNumDiv, true);\n";
    html += "      graphDiv.style.display = 'block'; disableInputs(graphDiv, false);\n";
    html += "      console.log('Set graphDiv display to block for screen '+screen);\n";
    html += "    }\n";
    html += "  }\n";
    html += "  toggleBgImageColor(screen);\n";
    html += "}\n";
    html += "function toggleBgImageColor(screen){\n";
    html += "  var sel = document.getElementById('bg_image_'+screen);\n";
    html += "  var colorDiv = document.getElementById('number_bg_color_div_'+screen);\n";
    html += "  var dualColorDiv = document.getElementById('dual_bg_color_div_'+screen);\n";
    html += "  var graphColorDiv = document.getElementById('graph_bg_color_div_'+screen);\n";
    html += "  if(sel && colorDiv){\n";
    html += "    colorDiv.style.display = (sel.value === 'Custom Color') ? 'block' : 'none';\n";
    html += "  }\n";
    html += "  if(sel && dualColorDiv){\n";
    html += "    dualColorDiv.style.display = (sel.value === 'Custom Color') ? 'block' : 'none';\n";
    html += "  }\n";
    html += "  if(sel && graphColorDiv){\n";
    html += "    graphColorDiv.style.display = (sel.value === 'Custom Color') ? 'block' : 'none';\n";
    html += "  }\n";
    html += "}\n";
    html += "document.addEventListener('DOMContentLoaded',function(){\n";
    html += "  var testMode = " + String(test_mode ? "true" : "false") + ";\n";
    // Pass display types and show_bottom flags to JavaScript
    html += "  var displayTypes = [";
    for (int s = 0; s < NUM_SCREENS; s++) {
        html += String(screen_configs[s].display_type);
        if (s < NUM_SCREENS - 1) html += ",";
    }
    html += "];\n";
    html += "  var showBottom = [";
    for (int s = 0; s < NUM_SCREENS; s++) {
        html += String(screen_configs[s].show_bottom ? "true" : "false");
        if (s < NUM_SCREENS - 1) html += ",";
    }
    html += "];\n";
    html += "  // Initialize all screen configs to show correct divs and disable hidden inputs FIRST\n";
    html += "  for(var s=0; s<" + String(NUM_SCREENS) + "; s++) toggleGaugeConfig(s);\n";
    html += "  // Now create test buttons - only for display types with calibration tables\n";
    html += "  for (var s = 0; s < " + String(NUM_SCREENS) + "; ++s) {\n";
    html += "    var displayType = displayTypes[s];\n";
    html += "    // Only GAUGE (0) and GAUGE_NUMBER (4) have calibration tables with test buttons\n";
    html += "    if (displayType !== 0 && displayType !== 4) continue;\n";
    html += "    var maxGauges = (displayType === 0 && showBottom[s]) ? 2 : 1; // GAUGE with bottom enabled has 2 gauges, otherwise 1\n";
    html += "    for (var g = 0; g < maxGauges; ++g) {\n";
    html += "      for (var p = 0; p < 5; ++p) {\n";
    html += "        var btn = document.createElement('button');\n";
    html += "        btn.type = 'button';\n";
    html += "        btn.innerText = 'Test';\n";
    html += "        btn.disabled = !testMode;\n";
    html += "        btn.style.cssText = 'padding:6px 12px;font-size:14px;font-weight:bold;background-color:'+(testMode?'#ff0000':'#999999')+';color:#ffffff;border:2px solid #000000;border-radius:4px;cursor:'+(testMode?'pointer':'not-allowed')+';display:inline-block;min-width:50px;position:relative;z-index:9999;';\n";
    html += "        btn.onclick = (function(ss,gg,pp){ return function(){\n";
    html += "          var angleInput = document.querySelector('input[name=\"angle_'+ss+'_'+gg+'_'+pp+'\"]');\n";
    html += "          var testAngle = document.getElementById('testangle_'+ss+'_'+gg+'_'+pp);\n";
    html += "          if(angleInput && testAngle){ testAngle.value = angleInput.value; }\n";
    html += "          document.getElementById('testform_'+ss+'_'+gg+'_'+pp).submit();\n";
    html += "        }; })(s,g,p);\n";
    html += "        var holder = document.getElementById('testbtn_'+s+'_'+g+'_'+p);\n";
    html += "        if(holder) {\n";
    html += "          holder.appendChild(btn);\n";
    html += "          if(g===0 && p===0) {\n";
    html += "            var divName = (displayType === 0) ? 'gaugeconfig_' : 'gaugenumconfig_';\n";
    html += "            var configDiv = document.getElementById(divName + s);\n";
    html += "            var btnStyle = window.getComputedStyle(btn);\n";
    html += "            console.log('Button for screen '+s+' (type '+displayType+'):', 'bgColor:', btnStyle.backgroundColor, 'color:', btnStyle.color, 'width:', btnStyle.width, 'height:', btnStyle.height, 'disabled:', btn.disabled);\n";
    html += "          }\n";
    html += "        } else {\n";
    html += "          console.log('No holder found for testbtn_'+s+'_'+g+'_'+p);\n";
    html += "        }\n";
    html += "      }\n";
    html += "    }\n";
    html += "  }\n";
    html += "  // Restore active tab from URL hash if present, else default to 0\n";
    html += "  var initial = 0; if(location.hash && location.hash.indexOf('#tab')===0){ initial = parseInt(location.hash.replace('#tab',''))||0; }\n";
    html += "  showScreenTab(initial);\n";
    html += "});</script>";
    html += "<p style='text-align:center;'><a href='/'>Back</a></p>";
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_save_gauges() {
            // Debug: print all POSTed keys and values
            Serial.println("[DEBUG] POSTED FORM KEYS AND VALUES:");
            int nArgs = config_server.args();
            for (int i = 0; i < nArgs; ++i) {
                String key = config_server.argName(i);
                String val = config_server.arg(i);
                Serial.printf("[POST] %s = '%s'\n", key.c_str(), val.c_str());
            }
        Serial.println("[DEBUG] handle_save_gauges() POST values and gauge_cal after POST:");
        for (int s = 0; s < NUM_SCREENS; ++s) {
            for (int g = 0; g < 2; ++g) {
                for (int p = 0; p < 5; ++p) {
                    String angleKey = "angle_" + String(s) + "_" + String(g) + "_" + String(p);
                    String valueKey = "value_" + String(s) + "_" + String(g) + "_" + String(p);
                    Serial.printf("[DEBUG] POST: %s='%s', %s='%s'\n", angleKey.c_str(), config_server.arg(angleKey).c_str(), valueKey.c_str(), config_server.arg(valueKey).c_str());
                    Serial.printf("[DEBUG] gauge_cal[%d][%d][%d]: angle=%d value=%.2f\n", s, g, p, gauge_cal[s][g][p].angle, gauge_cal[s][g][p].value);
                }
            }
        }
        Serial.println("[DEBUG] handle_gauges_page() - gauge_cal values sent to GUI:");
        for (int s = 0; s < NUM_SCREENS; ++s) {
            for (int g = 0; g < 2; ++g) {
                for (int p = 0; p < 5; ++p) {
                    Serial.printf("[DEBUG] gauge_cal[%d][%d][%d]: angle=%d value=%.2f\n", s, g, p, gauge_cal[s][g][p].angle, gauge_cal[s][g][p].value);
                }
            }
        }
    Serial.println("[DEBUG] handle_save_gauges() called");
    if (config_server.method() == HTTP_POST) {
        bool reboot_needed = false;
        for (int s = 0; s < NUM_SCREENS; ++s) {
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
                // Detect if the icon path actually changed; if so, request a reboot
                if (strncmp(screen_configs[s].icon_paths[g], iconValue.c_str(), 127) != 0) {
                    reboot_needed = true;
                }
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
                    }
                    
                    // Save number display settings
                    String numberPathKey = "number_path_" + String(s);
                    if (config_server.hasArg(numberPathKey)) {
                        strncpy(screen_configs[s].number_path, config_server.arg(numberPathKey).c_str(), 127);
                        screen_configs[s].number_path[127] = '\0';
                    }
                    
                    // Note: background is now in bg_image field (can be bin file path or "Custom Color")
                    String numberBgColorKey = "number_bg_color_" + String(s);
                    if (config_server.hasArg(numberBgColorKey)) {
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
                    
                    // Save dual display background color (uses same field as number display)
                    String dualBgColorKey = "dual_bg_color_" + String(s);
                    if (config_server.hasArg(dualBgColorKey)) {
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
                    
                    // Save quad display background color (uses same field as number/dual display)
                    String quadBgColorKey = "quad_bg_color_" + String(s);
                    if (config_server.hasArg(quadBgColorKey)) {
                        strncpy(screen_configs[s].number_bg_color, config_server.arg(quadBgColorKey).c_str(), 7);
                        screen_configs[s].number_bg_color[7] = '\0';
                    }
                    
                    // Save quad display settings (TL, TR, BL, BR)
                    auto saveQuadrant = [&](const char* name, char* path, uint8_t& size, char* color) {
                        String pathKey = "quad_" + String(name) + "_path_" + String(s);
                        if (config_server.hasArg(pathKey)) {
                            strncpy(path, config_server.arg(pathKey).c_str(), 127);
                            path[127] = '\0';
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
                    
                    saveQuadrant("tl", screen_configs[s].quad_tl_path, screen_configs[s].quad_tl_font_size, screen_configs[s].quad_tl_font_color);
                    saveQuadrant("tr", screen_configs[s].quad_tr_path, screen_configs[s].quad_tr_font_size, screen_configs[s].quad_tr_font_color);
                    saveQuadrant("bl", screen_configs[s].quad_bl_path, screen_configs[s].quad_bl_font_size, screen_configs[s].quad_bl_font_color);
                    saveQuadrant("br", screen_configs[s].quad_br_path, screen_configs[s].quad_br_font_size, screen_configs[s].quad_br_font_color);
                    
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
                }
                // Only process zone settings if the first zone field is in the form
                // (gauges hidden by display type won't submit their zone fields)
                String firstZoneKey = "mnv" + String(s) + String(g) + "1";
                if (config_server.hasArg(firstZoneKey)) {
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
                } else {
                    Serial.printf("[DEBUG] Skipping zones for screen %d gauge %d (not in form)\n", s, g);
                }
                // Only process calibration data if the angle fields are actually in the form
                // (gauges hidden by display type won't submit their fields)
                String firstAngleKey = "angle_" + String(s) + "_" + String(g) + "_0";
                if (config_server.hasArg(firstAngleKey)) {
                    for (int p = 0; p < 5; ++p) {
                        String angleKey = "angle_" + String(s) + "_" + String(g) + "_" + String(p);
                        String valueKey = "value_" + String(s) + "_" + String(g) + "_" + String(p);
                        String angleStr = config_server.arg(angleKey);
                        String valueStr = config_server.arg(valueKey);
                        Serial.printf("[DEBUG] POST: %s='%s', %s='%s'\n", angleKey.c_str(), angleStr.c_str(), valueKey.c_str(), valueStr.c_str());
                        gauge_cal[s][g][p].angle = angleStr.toInt();
                        gauge_cal[s][g][p].value = valueStr.toFloat();
                    }
                } else {
                    Serial.printf("[DEBUG] Skipping calibration for screen %d gauge %d (not in form)\n", s, g);
                }
            }
        }
        // Print updated gauge_cal values before saving
        Serial.println("[DEBUG] gauge_cal values after POST:");
        for (int s = 0; s < NUM_SCREENS; ++s) {
            for (int g = 0; g < 2; ++g) {
                for (int p = 0; p < 5; ++p) {
                    Serial.printf("[DEBUG] gauge_cal[%d][%d][%d]: angle=%d value=%.2f\n", s, g, p, gauge_cal[s][g][p].angle, gauge_cal[s][g][p].value);
                }
            }
        }
        // Attempt to write per-screen binary configs to SD immediately so toggles
        // (like show_bottom) persist even if NVS writes fail or are delayed.
        if (!SD_MMC.exists("/config")) SD_MMC.mkdir("/config");
        for (int s2 = 0; s2 < NUM_SCREENS; ++s2) {
            char sdpath[64];
            snprintf(sdpath, sizeof(sdpath), "/config/screen%d.bin", s2);
            File sf = SD_MMC.open(sdpath, FILE_WRITE);
            if (sf) {
                size_t wrote = sf.write((const uint8_t *)&screen_configs[s2], sizeof(ScreenConfig));
                sf.close();
                Serial.printf("[SD SAVE] Immediate wrote '%s' -> %u bytes\n", sdpath, (unsigned)wrote);
            } else {
                Serial.printf("[SD SAVE] Immediate failed to open '%s' for writing\n", sdpath);
            }
        }

        // Persist to NVS/Preferences as well
        save_preferences();

        // Refresh Signal K subscriptions immediately in case any SK paths changed
        // (safe to call even if WS not connected; function will no-op locally)
        refresh_signalk_subscriptions();
        
        // Fetch metadata for any new or changed paths
        fetch_all_metadata();

        // Prefer hot-apply: try to apply visuals now. If successful, skip reloading
        // stored preferences when rendering the gauges page so the user sees the
        // updated state immediately.
        bool applied_now = apply_all_screen_visuals();
        if (applied_now) {
            skip_next_load_preferences = true;
        } else {
            Serial.println("[HOTAPPLY] apply_all_screen_visuals() returned false — UI objects may not be present yet");
            // still set skip flag so the page reflects in-memory state; we will not reboot
            skip_next_load_preferences = true;
        }
        // Debug: print updated screen_configs including buzzer flags to verify checkboxes
        Serial.println("[DEBUG] Screen configs after save (including buzzer flags):");
        dump_screen_configs();
        
        // NVS key enumeration debug (after all saves, before reboot)
        {
            nvs_handle_t nvs_enum_handle;
            esp_err_t nvs_enum_err = nvs_open(PREF_NAMESPACE, NVS_READONLY, &nvs_enum_handle);
            if (nvs_enum_err == ESP_OK) {
                Serial.println("[NVS ENUM] Listing all keys in 'gaugeconfig' namespace after icon/zone save:");
                nvs_iterator_t it = NULL;
                esp_err_t enum_find_err = nvs_entry_find(NULL, PREF_NAMESPACE, NVS_TYPE_ANY, &it);
                while (enum_find_err == ESP_OK && it != NULL) {
                    nvs_entry_info_t info;
                    nvs_entry_info(it, &info);
                    Serial.printf("[NVS ENUM] key: %s, type: %d\n", info.key, info.type);
                    enum_find_err = nvs_entry_next(&it);
                }
                Serial.println("[NVS ENUM] End of key list.");
                nvs_close(nvs_enum_handle);
            } else {
                Serial.printf("[NVS ENUM] nvs_open failed: %d\n", nvs_enum_err);
            }
        }
        // Try to apply visual changes at runtime (hot-update). If LVGL objects are not present
        // or the update fails, fall back to reboot to ensure a clean load.
        if (reboot_needed) {
            bool applied = false;
            // attempt hot-apply
            applied = apply_all_screen_visuals();
            if (applied) {
                // Immediately reload the Gauge Calibration page instead of showing an intermediate page
                {
                    String redirectPath = "/gauges";
                    if (config_server.hasArg("active_tab")) {
                        String at = config_server.arg("active_tab");
                        at.trim();
                        if (at.length() > 0) redirectPath += "#tab" + at;
                    }
                    config_server.sendHeader("Location", redirectPath, true);
                    config_server.send(302, "text/plain", "");
                    return;
                }
            } else {
                String html = "<html><head>";
                html += STYLE;
                html += "<title>Rebooting</title></head><body><div class='container'>";
                html += "<h3>Applying changes requires reboot. Rebooting...</h3>";
                html += "</div></body></html>";
                config_server.send(200, "text/html", html);
                delay(250);
                ESP.restart();
                return;
            }
        } else {
            // Attempt to apply visual changes at runtime (positions, recolor etc.) even when no reboot needed
            bool applied_now = apply_all_screen_visuals();
            // Redirect back to the gauges page (no reboot), preserving active tab
            {
                String redirectPath = "/gauges";
                if (config_server.hasArg("active_tab")) {
                    String at = config_server.arg("active_tab");
                    at.trim();
                    if (at.length() > 0) redirectPath += "#tab" + at;
                }
                config_server.sendHeader("Location", redirectPath, true);
                config_server.send(302, "text/plain", "");
                return;
            }
        }
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
    html += "</div>"; // root-actions
    html += "</div>"; // tab-content
    html += "</div></body></html>";
    config_server.send(200, "text/html", html);
}

void handle_network_page() {
    String html = "<html><head>";
    html += STYLE;
    html += "<title>Network Setup</title></head><body><div class='container'>";
    html += "<div class='tab-content'>";
    html += "<h2>Network Setup</h2>";
    html += "<form method='POST' action='/save-wifi'>";
    html += "<div class='form-row'><label>SSID:</label><input name='ssid' type='text' value='" + saved_ssid + "'></div>";
    html += "<div class='form-row'><label>Password:</label><input name='password' type='password' value='" + saved_password + "'></div>";
    html += "<div class='form-row'><label>SignalK Server:</label><input name='signalk_ip' type='text' value='" + saved_signalk_ip + "'></div>";
    html += "<div class='form-row'><label>SignalK Port:</label><input name='signalk_port' type='number' value='" + String(saved_signalk_port) + "'></div>";
    html += "<div class='form-row'><label>ESP32 Hostname:</label><input name='hostname' type='text' value='" + saved_hostname + "'></div>";
    html += "<div style='text-align:center;margin-top:12px;'><button class='tab-btn' type='submit' style='padding:10px 18px;'>Save & Reboot</button></div>";
    html += "</form>";
    html += "<p style='text-align:center; margin-top:10px;'><a href='/'>Back</a></p>";
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
        first_run_buzzer = true;
        Serial.printf("[DEVICE SAVE_POST] buzzer_mode=%d buzzer_cooldown_sec=%u first_run_buzzer=%d auto_scroll=%u\n",
                  buzzer_mode, buzzer_cooldown_sec, (int)first_run_buzzer, (unsigned)auto_scroll_sec);

        auto_scroll_sec = asc;
        // Apply auto-scroll at runtime
        set_auto_scroll_interval(auto_scroll_sec);

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


void setup_sensESP() {
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
        WiFi.softAP("ESP32-SquareDisplay", "12345678");
        Serial.print("AP IP: ");
        Serial.println(WiFi.softAPIP());
    }
    // Show fallback error screen if needed (after config load, before UI init)
    show_fallback_error_screen_if_needed();

    // Register web UI routes and start server
    config_server.on("/", handle_root);
    config_server.on("/gauges", handle_gauges_page);
    config_server.on("/save-gauges", HTTP_POST, handle_save_gauges);
    config_server.on("/needles", handle_needles_page);
    config_server.on("/save-needles", HTTP_POST, handle_save_needles);
    // Assets manager page and upload/delete handlers
    config_server.on("/assets", handle_assets_page);
    config_server.on("/assets/upload", HTTP_POST, handle_assets_upload_post, handle_assets_upload);
    config_server.on("/assets/delete", HTTP_POST, handle_assets_delete);
    config_server.on("/network", handle_network_page);
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
    // Ensure assets dir exists (try SD_MMC, fall back to POSIX /sdcard)
    if (!SD_MMC.exists("/assets")) {
        SD_MMC.mkdir("/assets");
        if (!SD_MMC.exists("/assets")) {
            // Try POSIX mkdir on SDSPI VFS
            struct stat st;
            if (stat("/sdcard/assets", &st) != 0) {
                int rc = mkdir("/sdcard/assets", 0755);
                Serial.printf("[ASSETS] POSIX mkdir /sdcard/assets -> %d\n", rc);
            }
        }
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
    } else {
        // POSIX fallback: list files under /sdcard/assets
        DIR *d = opendir("/sdcard/assets");
        if (d) {
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                const char *fname = entry->d_name;
                if (!fname) continue;
                if (fname[0] == '.') continue; // skip . and .. and hidden
                String bname = String(fname);
                // get file size via stat
                char fullpath[256];
                snprintf(fullpath, sizeof(fullpath), "/sdcard/assets/%s", fname);
                struct stat st;
                long fsz = 0;
                if (stat(fullpath, &st) == 0) fsz = (long)st.st_size;
                html += "<tr><td>" + bname + "</td><td class='file-size'>" + String(fsz) + "</td>";
                html += "<td class='file-actions'><form method='POST' action='/assets/delete'><input type='hidden' name='file' value='" + bname + "'>";
                html += "<input type='submit' value='Delete' class='tab-btn' onclick='return confirm(\"Delete " + bname + "?\")'></form>";
                html += " <a href='S:/assets/" + bname + "' target='_blank' class='tab-btn' style='padding:6px 10px;text-decoration:none;'>Download</a></td></tr>";
            }
            closedir(d);
        }
    }
    html += "</table>";
    html += "<p style='text-align:center; margin-top:12px;'><a href='/'>Back</a></p>";
    html += "</div></div></body></html>";
    config_server.send(200, "text/html", html);
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
    // redirect back
    config_server.sendHeader("Location", "/assets");
    config_server.send(303, "text/plain", "");
}
