// n2k_gateway.cpp — see n2k_gateway.h

#include "n2k_gateway.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* PREFS_NS = "n2kgw";

static N2kGwSettings settings;
static bool settings_loaded = false;

static TaskHandle_t gw_task_handle = nullptr;
static volatile bool gw_running = false;
static N2kGwMsgHandler gw_handler = nullptr;
static volatile uint32_t gw_bad_lines = 0;

static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static char status_text[96] = "Not started";

static void set_status(const char* fmt, ...) {
    char tmp[sizeof(status_text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&status_mux);
    if (strcmp(tmp, status_text) != 0) {
        strcpy(status_text, tmp);
        portEXIT_CRITICAL(&status_mux);
        Serial.printf("[N2K-WiFi] %s\n", tmp);
        return;
    }
    portEXIT_CRITICAL(&status_mux);
}

String n2k_gw_status() {
    char tmp[sizeof(status_text)];
    portENTER_CRITICAL(&status_mux);
    strcpy(tmp, status_text);
    portEXIT_CRITICAL(&status_mux);
    return String(tmp);
}

uint32_t n2k_gw_bad_lines() { return gw_bad_lines; }

const char* n2k_gw_format_name(uint8_t format) {
    switch (format) {
        case N2KGW_FMT_YDRAW:     return "YD RAW (Yacht Devices)";
        case N2KGW_FMT_ACTISENSE: return "Actisense NGT binary";
        case N2KGW_FMT_SEASMART:  return "SeaSmart $PCDIN";
        default:                  return "Unknown";
    }
}

// ── Settings (own NVS namespace, separate from the main settings) ───────
void n2k_gw_load(N2kGwSettings& s) {
    Preferences p;
    s = N2kGwSettings();
    if (p.begin(PREFS_NS, true)) {
        s.use_wifi = p.getBool("use_wifi", false);
        s.format   = p.getUChar("fmt", N2KGW_FMT_YDRAW);
        s.udp      = p.getBool("udp", false);
        s.host     = p.getString("host", "");
        s.port     = p.getUShort("port", 0);
        p.end();
    }
    if (s.format > N2KGW_FMT_SEASMART) s.format = N2KGW_FMT_YDRAW;
}

void n2k_gw_save(const N2kGwSettings& s) {
    Preferences p;
    if (!p.begin(PREFS_NS, false)) {
        Serial.println("[N2K-WiFi] ERROR: could not open settings");
        return;
    }
    p.putBool("use_wifi", s.use_wifi);
    p.putUChar("fmt", s.format);
    p.putBool("udp", s.udp);
    p.putString("host", s.host);
    p.putUShort("port", s.port);
    p.end();
}

const N2kGwSettings& n2k_gw_settings() {
    if (!settings_loaded) {
        n2k_gw_load(settings);
        settings_loaded = true;
    }
    return settings;
}

bool n2k_gw_enabled() {
    const N2kGwSettings& s = n2k_gw_settings();
    return s.use_wifi && s.port != 0 && (s.udp || s.host.length() > 0);
}

// ── Receive task (core 0, like the CAN task) ────────────────────────────
static void gw_task(void*) {
    const N2kGwSettings& s = n2k_gw_settings();
    tN2kGatewayDecoder* dec = new tN2kGatewayDecoder((N2kGwFormat)s.format, gw_handler);
    WiFiClient client;
    WiFiUDP udp;
    bool udp_open = false;
    static uint8_t buf[1024];
    uint32_t next_attempt = 0;
    uint32_t last_rx = 0;
    bool had_data = false;

    set_status("Starting (%s)", n2k_gw_format_name(s.format));

    while (gw_running) {
        dec->poll();
        gw_bad_lines = dec->bad_lines();

        if (WiFi.status() != WL_CONNECTED) {
            set_status("Waiting for WiFi");
            if (client.connected()) client.stop();
            if (udp_open) { udp.stop(); udp_open = false; }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t got = 0;

        if (!s.udp) {
            if (!client.connected()) {
                if ((int32_t)(millis() - next_attempt) < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
                set_status("Connecting to %s:%u (TCP)", s.host.c_str(), s.port);
                if (client.connect(s.host.c_str(), s.port, 3000)) {
                    client.setNoDelay(true);
                    dec->reset();
                    last_rx = millis();
                    had_data = false;
                    set_status("Connected to %s:%u, waiting for data", s.host.c_str(), s.port);
                } else {
                    set_status("Can't reach %s:%u (TCP), retrying", s.host.c_str(), s.port);
                    next_attempt = millis() + 5000;
                    continue;
                }
            }
            int avail = client.available();
            if (avail > 0) {
                int n = client.read(buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
                if (n > 0) { dec->feed(buf, n); got = n; }
            } else if (millis() - last_rx > 30000) {
                set_status("No data from %s:%u for 30 s, reconnecting", s.host.c_str(), s.port);
                client.stop();
                next_attempt = millis() + 1000;
            }
        } else {
            if (!udp_open) {
                udp_open = udp.begin(s.port);
                if (!udp_open) {
                    set_status("Can't listen on UDP port %u", s.port);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
                last_rx = millis();
                had_data = false;
                set_status("Listening on UDP port %u, waiting for data", s.port);
            }
            while (udp.parsePacket() > 0) {
                int n;
                while ((n = udp.read(buf, sizeof(buf))) > 0) { dec->feed(buf, n); got += n; }
                if (got >= 4096) break;   // let the loop yield during bursts
            }
        }

        if (got) {
            last_rx = millis();
            if (!had_data) {
                had_data = true;
                if (s.udp) set_status("Receiving on UDP port %u", s.port);
                else       set_status("Receiving from %s:%u", s.host.c_str(), s.port);
            }
            vTaskDelay(pdMS_TO_TICKS(1));     // always yield so core 0 stays responsive
        } else {
            if (had_data && s.udp && millis() - last_rx > 10000) {
                had_data = false;
                set_status("Listening on UDP port %u, no data for 10 s", s.port);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    client.stop();
    if (udp_open) udp.stop();
    delete dec;
    set_status("Stopped");
    gw_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void n2k_gw_start(N2kGwMsgHandler handler) {
    if (gw_running) return;
    gw_handler = handler;
    gw_running = true;
    xTaskCreatePinnedToCore(gw_task, "N2K-WiFi", 8192, nullptr, 3, &gw_task_handle, 0);
    Serial.println("[N2K-WiFi] gateway task launched");
}

void n2k_gw_stop() {
    gw_running = false;
}
