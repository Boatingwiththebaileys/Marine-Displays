#include "signalk_config.h"
#include "network_setup.h"
#include "screen_config_c_api.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <map>

extern "C" int ui_get_current_screen(void);

// Extended sensor storage for paths beyond the 10 gauge slots
static std::map<String, float> extended_sensor_values;
static std::map<String, String> extended_sensor_units;
static std::map<String, String> extended_sensor_descriptions;

// Global array to hold all sensor values (10 parameters)
float g_sensor_values[TOTAL_PARAMS] = {
    0,        // SCREEN1_RPM
    313.15,   // SCREEN1_COOLANT_TEMP
    0,        // SCREEN2_RPM
    50.0,     // SCREEN2_FUEL
    313.15,   // SCREEN3_COOLANT_TEMP
    373.15,   // SCREEN3_EXHAUST_TEMP
    50.0,     // SCREEN4_FUEL
    313.15,   // SCREEN4_COOLANT_TEMP
    2.0,      // SCREEN5_OIL_PRESSURE
    313.15    // SCREEN5_COOLANT_TEMP
};

// Mutex for thread-safe access to sensor variables
SemaphoreHandle_t sensor_mutex = NULL;

// WiFi and HTTP client (static to this file)
static WebSocketsClient ws_client;
static String server_ip_str = "";
static uint16_t server_port_num = 0;
static String signalk_paths[TOTAL_PARAMS];  // Array of 10 paths
static TaskHandle_t signalk_task_handle = NULL;
static bool signalk_enabled = false;

// Connection health and reconnection/backoff state
static unsigned long last_message_time = 0;
static unsigned long last_reconnect_attempt = 0;
static unsigned long next_reconnect_at = 0;
static unsigned long current_backoff_ms = 2000; // start 2s
static const unsigned long RECONNECT_BASE_MS = 2000;
static const unsigned long RECONNECT_MAX_MS = 60000;
static const unsigned long MESSAGE_TIMEOUT_MS = 30000; // 30s without messages => reconnect
static const unsigned long PING_INTERVAL_MS = 15000; // send periodic ping
static const int SK_SUBSCRIBE_PERIOD_MS = 200; // request 5Hz updates from server

// Outgoing message queue (simple ring buffer)
static SemaphoreHandle_t ws_queue_mutex = NULL;
static const int OUTGOING_QUEUE_SIZE = 8;
static String outgoing_queue[OUTGOING_QUEUE_SIZE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

static bool enqueue_outgoing(const String &msg) {
    if (ws_queue_mutex == NULL) return false;
    if (xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) {
        if (queue_count >= OUTGOING_QUEUE_SIZE) {
            // Drop oldest to make room
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
    if (ws_queue_mutex == NULL) return;
    if (!ws_client.isConnected()) return;
    if (!xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) return;
    while (queue_count > 0 && ws_client.isConnected()) {
        String &m = outgoing_queue[queue_head];
        ws_client.sendTXT(m);
        queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
        queue_count--;
    }
    xSemaphoreGive(ws_queue_mutex);
}

// Public enqueue wrapper (declared in header)
void enqueue_signalk_message(const String &msg) {
    if (ws_queue_mutex == NULL) return;
    enqueue_outgoing(msg);
}

// Convert dot-delimited Signal K path to REST URL form
static String build_signalk_url(const String &path) {
    String cleaned = path;
    cleaned.trim();
    cleaned.replace(".", "/");
    return String("/signalk/v1/api/vessels/self/") + cleaned;
}

// Thread-safe getter for any sensor value
float get_sensor_value(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return 0;
    
    float val = 0;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        val = g_sensor_values[index];
        xSemaphoreGive(sensor_mutex);
    }
    return val;
}

// Thread-safe setter for any sensor value
void set_sensor_value(int index, float value) {
    if (index < 0 || index >= TOTAL_PARAMS) return;
    
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        float old = g_sensor_values[index];
        if (old != value) {
            g_sensor_values[index] = value;
        } else {
            // No change; keep as-is
        }
        xSemaphoreGive(sensor_mutex);
    }
}

// Initialize mutex
void init_sensor_mutex() {
    if (sensor_mutex == NULL) {
        sensor_mutex = xSemaphoreCreateMutex();
    }
}

// By-path lookup: check fixed gauge paths first, then extended storage
float get_sensor_value_by_path(const String& path) {
    if (path.length() == 0) return NAN;
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_value(i);
        }
    }
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_values.find(path);
        float val = (it != extended_sensor_values.end()) ? it->second : NAN;
        xSemaphoreGive(sensor_mutex);
        return val;
    }
    return NAN;
}

String get_sensor_unit_by_path(const String& path) {
    if (path.length() == 0) return "";
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_units.find(path);
        String unit = (it != extended_sensor_units.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return unit;
    }
    return "";
}

String get_sensor_description_by_path(const String& path) {
    if (path.length() == 0) return "";
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_descriptions.find(path);
        String desc = (it != extended_sensor_descriptions.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return desc;
    }
    return "";
}

// WebSocket event handler
static void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
        Serial.println("Signal K: WebSocket connected");
        last_message_time = millis();
        // reset backoff on successful connect
        current_backoff_ms = RECONNECT_BASE_MS;

        // Subscribe ONLY to the active screen's paths
        int active_scr = ui_get_current_screen();
        if (active_scr < 1) active_scr = 1;
        int idx = active_scr - 1;
        int base = idx * PARAMS_PER_SCREEN;

        String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
        bool first = true;
        for (int i = base; i < base + PARAMS_PER_SCREEN && i < TOTAL_PARAMS; i++) {
            if (signalk_paths[i].length() > 0) {
                if (!first) out += ",";
                out += "{\"path\":\"";
                out += signalk_paths[i];
                out += "\",\"period\":";
                out += String(SK_SUBSCRIBE_PERIOD_MS);
                out += "}";
                first = false;
            }
        }
        // Also include gauge_num_center_path if this screen uses it
        if (screen_configs[idx].display_type == DISPLAY_TYPE_GAUGE_NUMBER &&
            strlen(screen_configs[idx].gauge_num_center_path) > 0) {
            if (!first) out += ",";
            out += "{\"path\":\"";
            out += screen_configs[idx].gauge_num_center_path;
            out += "\",\"period\":";
            out += String(SK_SUBSCRIBE_PERIOD_MS);
            out += "}";
        }
        out += "]}";
        ws_client.sendTXT(out);
        Serial.printf("[SK] Subscribed to screen %d paths on connect\n", active_scr);
        // flush any queued outgoing messages (resubscribe, etc)
        flush_outgoing();
        return;
    }

    if (type == WStype_TEXT) {
        last_message_time = millis();
        String msg = String((char*)payload, length);
        // Parse incoming JSON and look for updates->values
        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc, msg);
        if (err) return;

        if (doc.containsKey("updates")) {
            JsonArray updates = doc["updates"].as<JsonArray>();
            for (JsonVariant update : updates) {
                if (!update.containsKey("values")) continue;
                JsonArray values = update["values"].as<JsonArray>();
                for (JsonVariant val : values) {
                    if (!val.containsKey("path") || !val.containsKey("value")) continue;
                    const char* path = val["path"];
                    float value = val["value"].as<float>();
                    bool matched_fixed = false;
                    for (int i = 0; i < TOTAL_PARAMS; i++) {
                        if (signalk_paths[i].length() > 0 && signalk_paths[i].equals(path)) {
                            set_sensor_value(i, value);
                            matched_fixed = true;
                        }
                    }
                    // Also store in extended maps (for gauge_num_center_path lookups)
                    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
                        extended_sensor_values[String(path)] = value;
                        xSemaphoreGive(sensor_mutex);
                    }
                }

                // Capture meta (units/descriptions) if present
                if (update.containsKey("meta")) {
                    JsonArray metas = update["meta"].as<JsonArray>();
                    for (JsonVariant m : metas) {
                        const char* mpath = m["path"] | "";
                        if (strlen(mpath) == 0) continue;
                        if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
                            if (m.containsKey("units")) {
                                extended_sensor_units[String(mpath)] = String(m["units"].as<const char*>());
                            }
                            if (m.containsKey("description")) {
                                extended_sensor_descriptions[String(mpath)] = String(m["description"].as<const char*>());
                            }
                            xSemaphoreGive(sensor_mutex);
                        }
                    }
                }
            }
        }
    }
    // handle pong or ping responses if available
    if (type == WStype_PONG) {
        last_message_time = millis();
        Serial.println("Signal K: received PONG");
    }
}

// FreeRTOS task for Signal K updates (runs on core 0)
// Task to run the WebSocket loop
static void signalk_task(void *parameter) {
    Serial.println("Signal K WebSocket task started");
    vTaskDelay(pdMS_TO_TICKS(500));

    while (signalk_enabled) {
        ws_client.loop();

        unsigned long now = millis();

        // send periodic ping if connected
        if (ws_client.isConnected()) {
            if (now - last_message_time >= PING_INTERVAL_MS) {
                ws_client.sendPing();
                Serial.println("Signal K: sent PING");
            }
        }

        // detect silent drop: no messages/pongs for MESSAGE_TIMEOUT_MS
        if (ws_client.isConnected()) {
            if (now - last_message_time >= MESSAGE_TIMEOUT_MS) {
                Serial.println("Signal K: connection idle timeout, forcing disconnect");
                ws_client.disconnect();
                // schedule reconnect with current_backoff_ms + jitter
                unsigned int jitter = (esp_random() & 0x7FF) % 1000; // up to 1s jitter
                next_reconnect_at = now + current_backoff_ms + jitter;
                last_reconnect_attempt = now;
                // increase backoff for next time
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        } else {
            // Not connected: attempt reconnect when scheduled
            if (next_reconnect_at == 0) {
                // first time; schedule immediate try
                next_reconnect_at = now + current_backoff_ms;
            }
            if (now >= next_reconnect_at) {
                Serial.println("Signal K: attempting reconnect...");
                // re-init client
                ws_client.begin(server_ip_str.c_str(), server_port_num, "/signalk/v1/stream?subscribe=none");
                ws_client.onEvent(wsEvent);
                last_reconnect_attempt = now;
                // schedule next if this fails
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("Signal K WebSocket task ended");
    vTaskDelete(NULL);
}

// Enable Signal K with WiFi credentials
void enable_signalk(const char* ssid, const char* password, const char* server_ip, uint16_t server_port) {
    if (signalk_enabled) {
        Serial.println("Signal K already enabled");
        return;
    }
    
    signalk_enabled = true;
    server_ip_str = server_ip;
    server_port_num = server_port;
    
    // Get all 10 paths from configuration (these are set in sensESP_setup)
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
        Serial.printf("=== ACTIVE Signal K path[%d] = '%s' ===\n", i, signalk_paths[i].c_str());
    }
    
    Serial.println("=== Signal K paths loaded from configuration ===");
    
    // Initialize mutex first
    init_sensor_mutex();
    // create ws queue mutex
    if (ws_queue_mutex == NULL) {
        ws_queue_mutex = xSemaphoreCreateMutex();
    }
    
    // WiFi should already be connected from setup_network()
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Signal K: WiFi not connected, aborting");
        signalk_enabled = false;
        return;
    }
    Serial.println("Signal K: Starting WebSocket client...");

    // Initialize websocket client
    // subscribe=none prevents server from firehosing all data on connect
    ws_client.begin(server_ip_str.c_str(), server_port_num, "/signalk/v1/stream?subscribe=none");
    ws_client.onEvent(wsEvent);
    // We'll manage reconnection with backoff ourselves
    ws_client.setReconnectInterval(0);

    // Create task to pump ws loop
    xTaskCreatePinnedToCore(
        signalk_task,
        "SignalKWS",
        8192,
        NULL,
        3,
        &signalk_task_handle,
        0
    );

    Serial.println("Signal K WebSocket task created successfully");
    Serial.flush();
}

// Disable Signal K
void disable_signalk() {
    signalk_enabled = false;
    if (signalk_task_handle != NULL) {
        vTaskDelete(signalk_task_handle);
        signalk_task_handle = NULL;
    }
    ws_client.disconnect();
    Serial.println("Signal K disabled (WebSocket disconnected)");
}

// Subscribe to only the active screen's SignalK paths.
// Sends an unsubscribe-all first, then subscribes to only the needed paths.
void subscribe_to_active_screen(int screen_1based) {
    if (screen_1based < 1) screen_1based = 1;
    int idx = screen_1based - 1;
    int base = idx * PARAMS_PER_SCREEN;

    // Unsubscribe from everything first
    String unsub = "{\"context\":\"vessels.self\",\"unsubscribe\":[{\"path\":\"*\"}]}";

    // Build subscribe for only this screen's paths
    String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
    bool first = true;
    for (int i = base; i < base + PARAMS_PER_SCREEN && i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i].length() > 0) {
            if (!first) out += ",";
            out += "{\"path\":\"";
            out += signalk_paths[i];
            out += "\",\"period\":";
            out += String(SK_SUBSCRIBE_PERIOD_MS);
            out += "}";
            first = false;
        }
    }
    // Also include gauge_num_center_path if this screen uses it
    if (idx < NUM_SCREENS &&
        screen_configs[idx].display_type == DISPLAY_TYPE_GAUGE_NUMBER &&
        strlen(screen_configs[idx].gauge_num_center_path) > 0) {
        if (!first) out += ",";
        out += "{\"path\":\"";
        out += screen_configs[idx].gauge_num_center_path;
        out += "\",\"period\":";
        out += String(SK_SUBSCRIBE_PERIOD_MS);
        out += "}";
    }
    out += "]}";

    if (ws_client.isConnected()) {
        ws_client.sendTXT(unsub);
        ws_client.sendTXT(out);
        Serial.printf("[SK] Subscribed to %d paths for screen %d\n",
                      PARAMS_PER_SCREEN, screen_1based);
    } else {
        enqueue_outgoing(unsub);
        enqueue_outgoing(out);
        Serial.printf("[SK] Queued subscription for screen %d (WS not connected)\n", screen_1based);
    }
}

// Rebuild the subscription list from current configuration and (re)send it
// over the active WebSocket connection if connected. If the WS is not
// connected, the updated paths will be used when connection is (re)established.
void refresh_signalk_subscriptions() {
    // Reload signalk_paths from configuration
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
        Serial.printf("[SignalK] refreshed path[%d] = '%s'\n", i, signalk_paths[i].c_str());
    }

    // Re-subscribe to only the active screen
    subscribe_to_active_screen(ui_get_current_screen());
}



