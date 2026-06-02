/*
 * SignalK WebSocket client for ESP32-S3-RLCD-4.2 Marine Display.
 *
 * Runs a dedicated FreeRTOS task on Core 0 that:
 *   1. Connects to the SignalK server via WebSocket
 *   2. Subscribes only to paths needed by the active screen (+ background graphs)
 *   3. Parses incoming delta messages and stores values in a path-keyed map
 *   4. Fetches metadata (units, description) via REST on first connect
 *   5. Reconnects with exponential backoff on disconnect
 */

#include "signalk_config.h"
#include "screen_config.h"
#include "screen_render.h"
#include "network_setup.h"
#include "audio_alert.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <map>
#include <set>
#include <vector>

// ─── PSRAM allocators ───────────────────────────────────────────────────────
// ESP32-S3-RLCD-4.2 has 8MB OPI PSRAM — use it for maps and JSON parsing
// to keep internal RAM free for WiFi/TCP buffers.

template <typename T>
struct PsramStlAllocator {
    using value_type = T;
    PsramStlAllocator() = default;
    template <class U> PsramStlAllocator(const PsramStlAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};
template <class T, class U>
bool operator==(const PsramStlAllocator<T>&, const PsramStlAllocator<U>&) { return true; }
template <class T, class U>
bool operator!=(const PsramStlAllocator<T>&, const PsramStlAllocator<U>&) { return false; }

template <typename K, typename V>
using PsramMap = std::map<K, V, std::less<K>,
    PsramStlAllocator<std::pair<const K, V>>>;

struct PsramAllocator {
    void* allocate(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* ptr) {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

// ─── Global state ───────────────────────────────────────────────────────────

SemaphoreHandle_t sensor_mutex = NULL;

// Navigation globals
volatile float g_nav_latitude  = NAN;
volatile float g_nav_longitude = NAN;
char g_sk_datetime[32]         = {0};

// Path-keyed value and metadata storage (all in PSRAM)
static PsramMap<String, float>  sensor_values;
static PsramMap<String, String> sensor_units;
static PsramMap<String, String> sensor_descriptions;

// WebSocket client
static WebSocketsClient ws_client;
static String server_ip_str = "";
static uint16_t server_port_num = 0;
static TaskHandle_t signalk_task_handle = NULL;
static bool signalk_enabled = false;

// Connection health / reconnection
static unsigned long last_message_time     = 0;
static unsigned long next_reconnect_at     = 0;
static unsigned long current_backoff_ms    = 2000;
static const unsigned long RECONNECT_BASE_MS  = 2000;
static const unsigned long RECONNECT_MAX_MS   = 60000;
static const unsigned long MESSAGE_TIMEOUT_MS = 30000;
static const unsigned long PING_INTERVAL_MS   = 15000;

// Outgoing message queue (ring buffer, Core-1 safe)
static SemaphoreHandle_t ws_queue_mutex = NULL;
static const int OUTGOING_QUEUE_SIZE = 8;
static String outgoing_queue[OUTGOING_QUEUE_SIZE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

// ─── Outgoing queue helpers ─────────────────────────────────────────────────

static bool enqueue_outgoing(const String &msg) {
    if (!ws_queue_mutex) return false;
    if (xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) {
        if (queue_count >= OUTGOING_QUEUE_SIZE) {
            queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
            queue_count--;
        }
        outgoing_queue[queue_tail] = msg;
        queue_tail = (queue_tail + 1) % OUTGOING_QUEUE_SIZE;
        queue_count++;
        xSemaphoreGive(ws_queue_mutex);
        return true;
    }
    return false;
}

static void flush_outgoing() {
    if (!ws_queue_mutex || !ws_client.isConnected()) return;
    if (!xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) return;
    while (queue_count > 0 && ws_client.isConnected()) {
        ws_client.sendTXT(outgoing_queue[queue_head]);
        queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
        queue_count--;
    }
    xSemaphoreGive(ws_queue_mutex);
}

// ─── Path collection helpers ────────────────────────────────────────────────

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

    const RlcdScreenConfig& cfg = screen_configs[s];
    switch (cfg.display_type) {
        case DISPLAY_TYPE_NUMBER:
            add(cfg.number_path);
            break;
        case DISPLAY_TYPE_DUAL:
            add(cfg.dual_top_path);
            add(cfg.dual_bottom_path);
            break;
        case DISPLAY_TYPE_QUAD:
            add(cfg.quad_tl_path);
            add(cfg.quad_tr_path);
            add(cfg.quad_bl_path);
            add(cfg.quad_br_path);
            break;
        case DISPLAY_TYPE_GRAPH:
            add(cfg.graph_path_1);
            add(cfg.graph_path_2);
            break;
        case DISPLAY_TYPE_COMPASS:
            add(cfg.compass_path);
            add(cfg.compass_bl_path);
            add(cfg.compass_br_path);
            break;
        case DISPLAY_TYPE_POSITION:
            add("navigation.position");
            add("navigation.datetime");
            break;
        case DISPLAY_TYPE_GAUGE:
            add(cfg.gauge_path);
            break;
    }
    return paths;
}

std::vector<String> get_all_signalk_paths() {
    std::set<String> unique;
    std::vector<String> all;
    for (int s = 0; s < NUM_SCREENS; s++) {
        for (const String& p : get_signalk_paths_for_screen(s)) {
            if (unique.find(p) == unique.end()) {
                unique.insert(p);
                all.push_back(p);
            }
        }
    }
    return all;
}

// Active screen paths + background graph screen paths + global alert paths
static std::vector<String> get_active_screen_paths(int screen_0based) {
    std::set<String> seen;
    std::vector<String> result;

    auto merge = [&](const std::vector<String>& src) {
        for (const String& p : src) {
            if (p.length() > 0 && seen.find(p) == seen.end()) {
                seen.insert(p);
                result.push_back(p);
            }
        }
    };

    merge(get_signalk_paths_for_screen(screen_0based));

    // Background graph screens still need data collection
    for (int s = 0; s < NUM_SCREENS; s++) {
        if (s == screen_0based) continue;
        if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH) {
            merge(get_signalk_paths_for_screen(s));
        }
    }

    // Global buzzer mode: also subscribe to alert-enabled paths from all screens
    if (buzzer_mode == 1) {
        for (int s = 0; s < NUM_SCREENS; s++) {
            if (s == screen_0based) continue;
            const RlcdScreenConfig& cfg = screen_configs[s];
            auto add_if_alert = [&](const char* path, int16_t lo, int16_t hi) {
                if ((lo != ALERT_OFF || hi != ALERT_OFF) && path[0]) {
                    String ps(path);
                    if (seen.find(ps) == seen.end()) {
                        seen.insert(ps);
                        result.push_back(ps);
                    }
                }
            };
            switch (cfg.display_type) {
                case DISPLAY_TYPE_NUMBER:
                    add_if_alert(cfg.number_path, cfg.number_alert_low, cfg.number_alert_high);
                    break;
                case DISPLAY_TYPE_DUAL:
                    add_if_alert(cfg.dual_top_path, cfg.dual_top_alert_low, cfg.dual_top_alert_high);
                    add_if_alert(cfg.dual_bottom_path, cfg.dual_bot_alert_low, cfg.dual_bot_alert_high);
                    break;
                case DISPLAY_TYPE_QUAD:
                    add_if_alert(cfg.quad_tl_path, cfg.quad_tl_alert_low, cfg.quad_tl_alert_high);
                    add_if_alert(cfg.quad_tr_path, cfg.quad_tr_alert_low, cfg.quad_tr_alert_high);
                    add_if_alert(cfg.quad_bl_path, cfg.quad_bl_alert_low, cfg.quad_bl_alert_high);
                    add_if_alert(cfg.quad_br_path, cfg.quad_br_alert_low, cfg.quad_br_alert_high);
                    break;
                case DISPLAY_TYPE_GAUGE:
                    add_if_alert(cfg.gauge_path, cfg.gauge_alert_low, cfg.gauge_alert_high);
                    break;
                default:
                    break;
            }
        }
    }

    return result;
}

// ─── Value access (thread-safe) ─────────────────────────────────────────────

float get_sensor_value_by_path(const String& path) {
    if (path.length() == 0) return NAN;
    if (sensor_mutex && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = sensor_values.find(path);
        float val = (it != sensor_values.end()) ? it->second : NAN;
        xSemaphoreGive(sensor_mutex);
        return val;
    }
    return NAN;
}

String get_sensor_unit_by_path(const String& path) {
    if (path.length() == 0) return "";
    if (sensor_mutex && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = sensor_units.find(path);
        String unit = (it != sensor_units.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return unit;
    }
    return "";
}

String get_sensor_description_by_path(const String& path) {
    if (path.length() == 0) return "";
    if (sensor_mutex && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = sensor_descriptions.find(path);
        String desc = (it != sensor_descriptions.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return desc;
    }
    return "";
}

// ─── Metadata fetching via REST API ─────────────────────────────────────────

static void fetch_metadata_for_path(const String& path) {
    if (path.length() == 0) return;

    String rest_path = path;
    rest_path.replace(".", "/");
    String url = "http://" + server_ip_str + ":" + String(server_port_num) +
                 "/signalk/v1/api/vessels/self/" + rest_path;

    esp_task_wdt_reset();
    HTTPClient http;
    http.setTimeout(1500);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        BasicJsonDocument<PsramAllocator> doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
        if (!err && doc.containsKey("meta")) {
            JsonObject meta = doc["meta"].as<JsonObject>();
            if (sensor_mutex && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
                if (meta.containsKey("units"))
                    sensor_units[path] = String(meta["units"].as<const char*>());
                if (meta.containsKey("description"))
                    sensor_descriptions[path] = String(meta["description"].as<const char*>());
                xSemaphoreGive(sensor_mutex);
            }
        }
    } else {
        Serial.printf("[SK] Metadata fetch failed for %s: %d\n", path.c_str(), httpCode);
    }
    http.end();
}

static void fetch_all_metadata() {
    std::vector<String> all = get_all_signalk_paths();
    for (const String& path : all) {
        esp_task_wdt_reset();
        fetch_metadata_for_path(path);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    Serial.printf("[SK] Fetched metadata for %d paths\n", (int)all.size());
}

// ─── WebSocket event handler ────────────────────────────────────────────────

static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) {
        Serial.println("[SK] WebSocket connected");
        last_message_time = millis();
        current_backoff_ms = RECONNECT_BASE_MS;

        // Subscribe to active screen paths
        int active = get_current_screen();
        std::vector<String> paths = get_active_screen_paths(active);
        String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
        bool first = true;
        for (const String& p : paths) {
            if (p.length() > 0) {
                if (!first) out += ",";
                out += "{\"path\":\"";
                out += p;
                out += "\",\"period\":0}";
                first = false;
            }
        }
        out += "]}";
        ws_client.sendTXT(out);
        flush_outgoing();

        // Fetch metadata via REST
        fetch_all_metadata();
        return;
    }

    if (type == WStype_TEXT) {
        last_message_time = millis();

        BasicJsonDocument<PsramAllocator> doc(4096);
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err) return;

        if (!doc.containsKey("updates")) return;

        JsonArray updates = doc["updates"].as<JsonArray>();
        for (JsonVariant update : updates) {
            if (!update.containsKey("values")) continue;
            JsonArray values = update["values"].as<JsonArray>();
            for (JsonVariant val : values) {
                if (!val.containsKey("path") || !val.containsKey("value")) continue;
                const char* path = val["path"];

                // navigation.position → {latitude, longitude}
                if (strcmp(path, "navigation.position") == 0) {
                    if (val["value"].is<JsonObject>()) {
                        JsonObject pos = val["value"].as<JsonObject>();
                        if (pos.containsKey("latitude"))  g_nav_latitude  = pos["latitude"].as<float>();
                        if (pos.containsKey("longitude")) g_nav_longitude = pos["longitude"].as<float>();
                    }
                    continue;
                }
                // navigation.datetime → ISO-8601 string
                if (strcmp(path, "navigation.datetime") == 0) {
                    const char* dt = val["value"].as<const char*>();
                    if (dt) { strncpy(g_sk_datetime, dt, 31); g_sk_datetime[31] = '\0'; }
                    continue;
                }

                // Numeric values → store by path
                float value = val["value"].as<float>();
                if (sensor_mutex && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
                    sensor_values[String(path)] = value;
                    xSemaphoreGive(sensor_mutex);
                }
            }
        }
    }

    if (type == WStype_PONG) {
        last_message_time = millis();
    }

    if (type == WStype_DISCONNECTED) {
        Serial.println("[SK] WebSocket disconnected");
        current_backoff_ms = RECONNECT_BASE_MS;
        next_reconnect_at = millis() + RECONNECT_BASE_MS;
    }

    if (type == WStype_ERROR) {
        Serial.println("[SK] WebSocket error");
        current_backoff_ms = RECONNECT_BASE_MS;
        next_reconnect_at = millis() + RECONNECT_BASE_MS;
    }
}

// ─── WS connection helper ───────────────────────────────────────────────────

static void ws_begin_connection() {
    ws_client.begin(server_ip_str.c_str(), server_port_num,
                    "/signalk/v1/stream?subscribe=none");
    ws_client.onEvent(wsEvent);
}

// ─── FreeRTOS task (Core 0) ─────────────────────────────────────────────────

static void signalk_task(void* parameter) {
    Serial.println("[SK] Task started");
    vTaskDelay(pdMS_TO_TICKS(500));

    while (signalk_enabled) {
        ws_client.loop();
        flush_outgoing();

        unsigned long now = millis();

        if (ws_client.isConnected()) {
            if (now - last_message_time >= PING_INTERVAL_MS) {
                ws_client.sendPing();
            }
            if (now - last_message_time >= MESSAGE_TIMEOUT_MS) {
                Serial.println("[SK] Idle timeout, forcing reconnect");
                ws_client.disconnect();
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        } else {
            if (next_reconnect_at == 0) {
                next_reconnect_at = now + current_backoff_ms;
            }
            if (now >= next_reconnect_at) {
                Serial.printf("[SK] Reconnecting to %s:%d...\n",
                              server_ip_str.c_str(), server_port_num);
                ws_begin_connection();
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("[SK] Task ended");
    vTaskDelete(NULL);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void enable_signalk(const char* server_ip, uint16_t server_port) {
    if (signalk_enabled) return;

    signalk_enabled = true;
    server_ip_str = server_ip;
    server_port_num = server_port;

    if (!sensor_mutex) sensor_mutex = xSemaphoreCreateMutex();
    if (!ws_queue_mutex) ws_queue_mutex = xSemaphoreCreateMutex();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[SK] WiFi not connected, aborting");
        signalk_enabled = false;
        return;
    }

    Serial.printf("[SK] Starting WebSocket to %s:%d\n", server_ip, server_port);
    ws_begin_connection();
    ws_client.setReconnectInterval(0);

    xTaskCreatePinnedToCore(signalk_task, "SignalKWS", 8192, NULL, 3,
                            &signalk_task_handle, 0);
    Serial.println("[SK] Task created");
}

void disable_signalk() {
    signalk_enabled = false;
    if (signalk_task_handle) {
        vTaskDelete(signalk_task_handle);
        signalk_task_handle = NULL;
    }
    ws_client.disconnect();
    Serial.println("[SK] Disabled");
}

void subscribe_to_active_screen(int screen_0based) {
    std::vector<String> paths = get_active_screen_paths(screen_0based);

    String unsub = "{\"context\":\"vessels.self\",\"unsubscribe\":[{\"path\":\"*\"}]}";
    enqueue_outgoing(unsub);

    String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
    bool first = true;
    for (const String& p : paths) {
        if (!first) out += ",";
        out += "{\"path\":\"";
        out += p;
        out += "\",\"period\":0}";
        first = false;
    }
    out += "]}";
    enqueue_outgoing(out);
    Serial.printf("[SK] Subscribed to %d paths for screen %d\n",
                  (int)paths.size(), screen_0based);
}

void refresh_signalk_subscriptions() {
    int active = get_current_screen();
    std::vector<String> paths = get_active_screen_paths(active);

    String unsub = "{\"context\":\"vessels.self\",\"unsubscribe\":[{\"path\":\"*\"}]}";
    enqueue_outgoing(unsub);

    String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
    bool first = true;
    for (const String& p : paths) {
        if (p.length() > 0) {
            if (!first) out += ",";
            out += "{\"path\":\"";
            out += p;
            out += "\",\"period\":0}";
            first = false;
        }
    }
    out += "]}";
    enqueue_outgoing(out);
    Serial.printf("[SK] Refreshed subscriptions (%d paths)\n", (int)paths.size());
}
