#!/usr/bin/env python3
"""
Fix remaining AsyncWebServer migration issues in network_setup.cpp:
- Add arg/hasArg lambdas to handlers that need them
- Replace direct LVGL calls with deferred flags
- Fix handle_assets_page to use AsyncResponseStream
- Rewrite handle_assets_upload to async multipart signature
"""
import os, sys

SRC = os.path.join(os.path.dirname(__file__), "..", "src", "network_setup.cpp")

LAMBDA = (
    '    auto arg = [&](const String& key) -> String {\n'
    '        auto* p = request->getParam(key, true);\n'
    '        if (!p) p = request->getParam(key, false);\n'
    '        return p ? p->value() : String("");\n'
    '    };\n'
    '    auto hasArg = [&](const String& key) -> bool {\n'
    '        return request->hasParam(key, true) || request->hasParam(key, false);\n'
    '    };\n'
)

replacements = []

# ── handle_save_wifi ─────────────────────────────────────────────────────────
# Remove method check wrapper, add lambda, route is POST-only
replacements.append((
    'void handle_save_wifi(AsyncWebServerRequest *request) {\n'
    '    if (request->method() == HTTP_POST) {\n'
    '        saved_ssid = arg("ssid");',
    'void handle_save_wifi(AsyncWebServerRequest *request) {\n'
    '    // Registered for HTTP_POST only.\n'
    + LAMBDA +
    '    saved_ssid = arg("ssid");'
))

# Remove the else + closing brace for old method check in save_wifi
replacements.append((
    '        ESP.restart();\n'
    '    } else {\n'
    '        request->send(405, "text/plain", "Method Not Allowed");\n'
    '    }\n'
    '}\n'
    '\n'
    'void handle_device_page',
    '        ESP.restart();\n'
    '}\n'
    '\n'
    'void handle_device_page'
))

# ── handle_save_device ───────────────────────────────────────────────────────
replacements.append((
    'void handle_save_device(AsyncWebServerRequest *request) {\n'
    '    if (request->method() == HTTP_POST) {\n'
    '        // Read and apply posted values\n'
    '        int bm = arg("buzzer_mode").toInt();',
    'void handle_save_device(AsyncWebServerRequest *request) {\n'
    '    // Registered for HTTP_POST only.\n'
    + LAMBDA +
    '    // Read and apply posted values\n'
    '    int bm = arg("buzzer_mode").toInt();'
))

# Replace set_auto_scroll_interval() with deferred flag
replacements.append((
    '        auto_scroll_sec = asc;\n'
    '        // Apply auto-scroll at runtime\n'
    '        set_auto_scroll_interval(auto_scroll_sec);\n'
    '\n'
    '        // Persist settings\n'
    '        save_preferences();\n'
    '\n'
    '        // Redirect back to device page\n'
    '        request->redirect("/device");\n'
    '        return;\n'
    '    }\n'
    '    request->send(405, "text/plain", "Method Not Allowed");\n'
    '}',
    '    auto_scroll_sec = asc;\n'
    '    // Defer auto-scroll update to Core 1 (LVGL runs there)\n'
    '    g_pending_auto_scroll_update = true;\n'
    '    g_pending_auto_scroll_sec = asc;\n'
    '\n'
    '    // Persist settings\n'
    '    save_preferences();\n'
    '\n'
    '    // Redirect back to device page\n'
    '    request->redirect("/device");\n'
    '}'
))

# ── handle_needles_page ──────────────────────────────────────────────────────
replacements.append((
    'void handle_needles_page(AsyncWebServerRequest *request) {\n'
    '    if (request->method() != HTTP_GET) {\n'
    '        request->send(405, "text/plain", "Method Not Allowed");\n'
    '        return;\n'
    '    }\n'
    '    int screen = 0;\n'
    '    int gauge = 0;\n'
    '    if (hasArg("screen")) screen = arg("screen").toInt();',
    'void handle_needles_page(AsyncWebServerRequest *request) {\n'
    '    if (request->method() != HTTP_GET) {\n'
    '        request->send(405, "text/plain", "Method Not Allowed");\n'
    '        return;\n'
    '    }\n'
    + LAMBDA +
    '    int screen = 0;\n'
    '    int gauge = 0;\n'
    '    if (hasArg("screen")) screen = arg("screen").toInt();'
))

# ── handle_save_needles ──────────────────────────────────────────────────────
replacements.append((
    'void handle_save_needles(AsyncWebServerRequest *request) {\n'
    '    if (request->method() != HTTP_POST) {\n'
    '        request->send(405, "text/plain", "Method Not Allowed");\n'
    '        return;\n'
    '    }\n'
    '    int screen = arg("screen").toInt();',
    'void handle_save_needles(AsyncWebServerRequest *request) {\n'
    '    if (request->method() != HTTP_POST) {\n'
    '        request->send(405, "text/plain", "Method Not Allowed");\n'
    '        return;\n'
    '    }\n'
    + LAMBDA +
    '    int screen = arg("screen").toInt();'
))

# Replace apply_all_needle_styles() with deferred flag in handle_save_needles
replacements.append((
    '    save_needle_style_from_args(screen, gauge, color, (uint16_t)width, (int16_t)inner, (int16_t)outer, (uint16_t)cx, (uint16_t)cy, rounded, gradient, fg);\n'
    '\n'
    '    // Apply immediately\n'
    '    apply_all_needle_styles();',
    '    save_needle_style_from_args(screen, gauge, color, (uint16_t)width, (int16_t)inner, (int16_t)outer, (uint16_t)cx, (uint16_t)cy, rounded, gradient, fg);\n'
    '\n'
    '    // Defer LVGL apply to Core 1 (handle_toggle_test_mode and loop() pick up this flag)\n'
    '    g_pending_apply_needles = true;'
))

# ── handle_test_gauge ────────────────────────────────────────────────────────
replacements.append((
    'void handle_test_gauge(AsyncWebServerRequest *request) {\n'
    '    if (request->method() == HTTP_POST) {\n'
    '        int screen = arg("screen").toInt();\n'
    '        int gauge = arg("gauge").toInt();\n'
    '        int point = arg("point").toInt();\n'
    '        int angle = hasArg("angle") ? arg("angle").toInt() : gauge_cal[screen][gauge][point].angle;\n'
    '        extern void test_move_gauge(int screen, int gauge, int angle);\n'
    '        extern bool test_mode;\n'
    '        test_mode = true;\n'
    '        test_move_gauge(screen, gauge, angle);\n'
    '        // Respond with 204 No Content so the UI does not change\n'
    '        request->send(204, "text/plain", "");\n'
    '    } else {\n'
    '        request->send(405, "text/plain", "Method Not Allowed");\n'
    '    }\n'
    '}',
    'void handle_test_gauge(AsyncWebServerRequest *request) {\n'
    '    // Registered for HTTP_POST only.\n'
    + LAMBDA +
    '    int screen = arg("screen").toInt();\n'
    '    int gauge  = arg("gauge").toInt();\n'
    '    int point  = arg("point").toInt();\n'
    '    int angle  = hasArg("angle") ? arg("angle").toInt() : gauge_cal[screen][gauge][point].angle;\n'
    '    // Defer LVGL gauge move to Core 1 (loop() processes these flags)\n'
    '    g_pending_test_gauge      = true;\n'
    '    g_pending_test_screen_idx = screen;\n'
    '    g_pending_test_gauge_idx  = gauge;\n'
    '    g_pending_test_angle      = angle;\n'
    '    request->send(204, "text/plain", "");\n'
    '}'
))

# ── handle_set_screen ────────────────────────────────────────────────────────
replacements.append((
    'void handle_set_screen(AsyncWebServerRequest *request) {\n'
    '    if (request->method() == HTTP_GET) {\n'
    '        int s = arg("screen").toInt();\n'
    '        if (s < 1 || s > NUM_SCREENS) s = 1;\n'
    '        // Call UI C API to change screen (1-5)\n'
    '        ui_set_screen(s);\n'
    '        // Redirect back to root so web UI reflects current screen\n'
    '        request->redirect("/");\n'
    '        return;\n'
    '    }\n'
    '    request->send(405, "text/plain", "Method Not Allowed");\n'
    '}',
    'void handle_set_screen(AsyncWebServerRequest *request) {\n'
    + LAMBDA +
    '    int s = arg("screen").toInt();\n'
    '    if (s < 1 || s > NUM_SCREENS) s = 1;\n'
    '    // Defer screen change to Core 1 (LVGL runs there)\n'
    '    g_pending_set_screen_idx = s;\n'
    '    request->redirect("/");\n'
    '}'
))

# ── handle_assets_delete ─────────────────────────────────────────────────────
replacements.append((
    'void handle_assets_delete(AsyncWebServerRequest *request) {\n'
    '    if (request->method() != HTTP_POST) { request->send(405, "text/plain", "Method Not Allowed"); return; }\n'
    '    String fname = arg("file");',
    'void handle_assets_delete(AsyncWebServerRequest *request) {\n'
    '    if (request->method() != HTTP_POST) { request->send(405, "text/plain", "Method Not Allowed"); return; }\n'
    + LAMBDA +
    '    String fname = arg("file");'
))

# Fix the redirect in handle_assets_delete (was sendHeader+send, now use redirect)
replacements.append((
    '    // redirect back\n'
    '    request->sendHeader("Location", "/assets");\n'
    '    request->send(303, "text/plain", "");\n'
    '}',
    '    request->redirect("/assets");\n'
    '}'
))

# ── handle_assets_page ───────────────────────────────────────────────────────
# Replace old streaming pattern with AsyncResponseStream
replacements.append((
    '// Assets manager: list files and show upload form\n'
    'void handle_assets_page(AsyncWebServerRequest *request) {\n'
    '    // Use cached file list \u2014 avoids SD scan during HTTP handling (SD/WiFi DMA conflict).\n'
    '    // Merged icon + bg into a single list for display.\n'
    '    request->send(200, "text/html; charset=utf-8", "");\n'
    '    String html;\n'
    '    html.reserve(4096);\n'
    '    auto flush = [&]() {\n'
    '        if (html.length() > 0) {\n'
    '            config_server.sendContent(html);\n'
    '            html.clear();\n'
    '            // Do NOT call Lvgl_Loop() here \u2014 re-entrant heap corruption.\n'
    '        }\n'
    '    };',
    '// Assets manager: list files and show upload form\n'
    'void handle_assets_page(AsyncWebServerRequest *request) {\n'
    '    // Use cached file list \u2014 avoids SD scan during HTTP handling (SD/WiFi DMA conflict).\n'
    '    // Merged icon + bg into a single list for display.\n'
    '    AsyncResponseStream *response = request->beginResponseStream("text/html; charset=utf-8");\n'
    '    String html;\n'
    '    html.reserve(4096);\n'
    '    auto flush = [&]() {\n'
    '        if (html.length() > 0) {\n'
    '            response->print(html);\n'
    '            html.clear();\n'
    '        }\n'
    '    };'
))

# Add request->send(response) at the end of handle_assets_page
replacements.append((
    '    html += "<p style=\'text-align:center; margin-top:12px;\'><a href=\'/\'>Back</a></p>";\n'
    '    html += "</div></div></body></html>";\n'
    '    flush();\n'
    '}\n'
    '\n'
    '// Upload handler: called during multipart upload',
    '    html += "<p style=\'text-align:center; margin-top:12px;\'><a href=\'/\'>Back</a></p>";\n'
    '    html += "</div></div></body></html>";\n'
    '    flush();\n'
    '    request->send(response);\n'
    '}\n'
    '\n'
    '// Upload handler: called during multipart upload'
))

# ── handle_assets_upload ─────────────────────────────────────────────────────
# Completely replace with async multipart signature
OLD_UPLOAD = (
    'void handle_assets_upload(AsyncWebServerRequest *request) {\n'
    '    HTTPUpload& upload = config_server.upload();\n'
    '    if (upload.status == UPLOAD_FILE_START) {\n'
    '        String filename = upload.filename;\n'
    '        // sanitize filename: remove paths\n'
    '        int slash = filename.lastIndexOf(\'/\');\n'
    '        if (slash >= 0) filename = filename.substring(slash + 1);\n'
    '        String path = String("/assets/") + filename;\n'
    '        Serial.printf("[ASSETS] Upload start: %s -> %s\\n", upload.filename.c_str(), path.c_str());\n'
    '        // open file for write (overwrite)\n'
    '        assets_upload_file = SD_MMC.open(path, FILE_WRITE);\n'
    '        if (!assets_upload_file) {\n'
    '            Serial.printf("[ASSETS] SD_MMC open failed for %s, trying POSIX fallback\\n", path.c_str());\n'
    '            // Try POSIX fopen on /sdcard prefix (SDSPI mount uses /sdcard)\n'
    '            String alt = String("/sdcard") + path;\n'
    '            assets_upload_fp = fopen(alt.c_str(), "wb");\n'
    '            if (!assets_upload_fp) {\n'
    '                Serial.printf("[ASSETS] POSIX fopen fallback failed for %s\\n", alt.c_str());\n'
    '            } else {\n'
    '                Serial.printf("[ASSETS] POSIX fopen fallback opened %s\\n", alt.c_str());\n'
    '            }\n'
    '        }\n'
    '    } else if (upload.status == UPLOAD_FILE_WRITE) {\n'
    '        if (assets_upload_file) {\n'
    '            assets_upload_file.write(upload.buf, upload.currentSize);\n'
    '        } else if (assets_upload_fp) {\n'
    '            fwrite(upload.buf, 1, upload.currentSize, assets_upload_fp);\n'
    '        }\n'
    '    } else if (upload.status == UPLOAD_FILE_END) {\n'
    '        if (assets_upload_file) {\n'
    '            assets_upload_file.close();\n'
    '            Serial.printf("[ASSETS] Upload finished (SD_MMC): %s (%u bytes)\\n", upload.filename.c_str(), (unsigned)upload.totalSize);\n'
    '        } else if (assets_upload_fp) {\n'
    '            fclose(assets_upload_fp);\n'
    '            assets_upload_fp = NULL;\n'
    '            Serial.printf("[ASSETS] Upload finished (POSIX fallback): %s (%u bytes)\\n", upload.filename.c_str(), (unsigned)upload.totalSize);\n'
    '        }\n'
    '    }\n'
    '}'
)

NEW_UPLOAD = (
    'void handle_assets_upload(AsyncWebServerRequest *request, const String& filename,\n'
    '                          size_t index, uint8_t *data, size_t len, bool final) {\n'
    '    if (index == 0) {\n'
    '        // First chunk: open destination file\n'
    '        String fname = filename;\n'
    '        int slash = fname.lastIndexOf(\'/\');\n'
    '        if (slash >= 0) fname = fname.substring(slash + 1);\n'
    '        String path = String("/assets/") + fname;\n'
    '        Serial.printf("[ASSETS] Upload start: %s -> %s\\n", filename.c_str(), path.c_str());\n'
    '        assets_upload_file = SD_MMC.open(path, FILE_WRITE);\n'
    '        if (!assets_upload_file) {\n'
    '            Serial.printf("[ASSETS] SD_MMC open failed for %s, trying POSIX fallback\\n", path.c_str());\n'
    '            String alt = String("/sdcard") + path;\n'
    '            assets_upload_fp = fopen(alt.c_str(), "wb");\n'
    '            if (!assets_upload_fp) {\n'
    '                Serial.printf("[ASSETS] POSIX fopen fallback failed for %s\\n", alt.c_str());\n'
    '            } else {\n'
    '                Serial.printf("[ASSETS] POSIX fopen fallback opened %s\\n", alt.c_str());\n'
    '            }\n'
    '        }\n'
    '    }\n'
    '    // Write this chunk\n'
    '    if (assets_upload_file) {\n'
    '        assets_upload_file.write(data, len);\n'
    '    } else if (assets_upload_fp) {\n'
    '        fwrite(data, 1, len, assets_upload_fp);\n'
    '    }\n'
    '    if (final) {\n'
    '        // Last chunk: close file\n'
    '        if (assets_upload_file) {\n'
    '            assets_upload_file.close();\n'
    '            Serial.printf("[ASSETS] Upload finished (SD_MMC): %s (%u bytes)\\n", filename.c_str(), (unsigned)(index + len));\n'
    '        } else if (assets_upload_fp) {\n'
    '            fclose(assets_upload_fp);\n'
    '            assets_upload_fp = NULL;\n'
    '            Serial.printf("[ASSETS] Upload finished (POSIX): %s (%u bytes)\\n", filename.c_str(), (unsigned)(index + len));\n'
    '        }\n'
    '    }\n'
    '}'
)
replacements.append((OLD_UPLOAD, NEW_UPLOAD))

# ── Apply all replacements ────────────────────────────────────────────────────
with open(SRC, "r") as f:
    text = f.read()

errors = 0
for old, new in replacements:
    if old in text:
        text = text.replace(old, new, 1)
        print(f"  OK: replaced {repr(old[:60])}...")
    else:
        print(f"  MISS: not found: {repr(old[:60])}...")
        errors += 1

with open(SRC, "w") as f:
    f.write(text)

print(f"\n{'DONE' if not errors else 'DONE WITH MISSES'}: {len(replacements)-errors}/{len(replacements)} applied")
sys.exit(0 if not errors == 0 else 0)
