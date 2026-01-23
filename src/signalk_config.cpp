#include "signalk_config.h"
#include "sensESP_setup.h"
#include <WiFi.h>
#include <esp_wifi.h>

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
static WiFiClient signalk_client;
static String server_ip_str = "";
static uint16_t server_port_num = 0;
static String signalk_paths[TOTAL_PARAMS];  // Array of 10 paths
static TaskHandle_t signalk_task_handle = NULL;
static bool signalk_enabled = false;

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

// Simple helper to poll one Signal K path and parse its value
static bool poll_signalk_path(const String &path, const char *label, int value_index, int &miss_counter, int &dbg_counter) {
    if (path.length() == 0 || path[0] == 0) {
        return false;
    }

    unsigned long start_ms = millis();

    signalk_client.setTimeout(2500);
    if (!signalk_client.connect(server_ip_str.c_str(), server_port_num, 2500)) {
        return false;
    }

    String url = build_signalk_url(path);
    signalk_client.print("GET " + url + " HTTP/1.1\r\n");
    signalk_client.print("Host: ");
    signalk_client.print(server_ip_str);
    signalk_client.print(":");
    signalk_client.println(server_port_num);
    signalk_client.println("Connection: close\r\n\r\n");
    signalk_client.flush();

    String response = "";
    unsigned long start_time = millis();
    bool header_done = false;
    bool value_seen = false;
    int bytes_after_value = 0;

    // Read for max 2500ms total, exit early when enough data after "value": received
    while (signalk_client.connected() && (millis() - start_time < 2500)) {
        while (signalk_client.available()) {
            char c = signalk_client.read();
            response += c;

            if (!header_done && response.indexOf("\r\n\r\n") != -1) {
                header_done = true;
            }

            if (!value_seen && response.indexOf("\"value\":") != -1) {
                value_seen = true;
            }

            if (value_seen) {
                bytes_after_value++;
                // Read at least 20 more bytes after seeing "value": to ensure we get the number
                if (bytes_after_value > 20) {
                    break;
                }
            }

            if (response.length() > 2000) {
                break;
            }
        }
        if (value_seen && bytes_after_value > 20) break;
        if (response.length() > 2000) break;
        delay(2);
    }

    signalk_client.stop();

    unsigned long elapsed_ms = millis() - start_ms;

    if (response.indexOf("404 Not Found") != -1) {
        if (miss_counter % 5 == 0) {
            Serial.println("Signal K 404: check path name (use dots; mapped to slashes)");
        }
        miss_counter++;
        return false;
    }

    int value_pos = response.lastIndexOf("\"value\":");
    if (value_pos == -1) {
        miss_counter++;
        return false;
    }

    int comma_pos = response.indexOf(",", value_pos);
    if (comma_pos == -1) comma_pos = response.indexOf("}", value_pos);
    if (comma_pos == -1) {
        miss_counter++;
        return false;
    }

    String value_str = response.substring(value_pos + 8, comma_pos);
    value_str.trim();
    float value = value_str.toFloat();

    // Debug snippet logging removed for production builds

    set_sensor_value(value_index, value);
    miss_counter = 0;
    Serial.print(label);
    Serial.println(value);

    return true;
}

// FreeRTOS task for Signal K updates (runs on core 0)
static void signalk_task(void *parameter) {
    Serial.println("Signal K task started on core 0");
    
    // Add safety delay and checks before using paths
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Verify server IP is configured
    if (server_ip_str.length() == 0) {
        Serial.println("Signal K: Server IP not configured, task exiting");
        vTaskDelete(NULL);
        return;
    }
    
    // Log configured paths (may be empty if not set)
    Serial.print("Signal K Server: ");
    Serial.print(server_ip_str);
    Serial.print(":");
    Serial.println(server_port_num);
    Serial.println("Configured Signal K paths:");
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        Serial.print("  Path[");
        Serial.print(i);
        Serial.print("]: '");
        Serial.print(signalk_paths[i]);
        Serial.println("'");
    }
    Serial.flush();
    
    // Wait for WiFi to stabilize
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Only proceed if WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Signal K: WiFi not connected, task exiting");
        vTaskDelete(NULL);
        return;
    }
    
    Serial.println("Signal K: WiFi connected, starting polling");
    Serial.flush();
    
    // Static counters for each path
    static int miss_counters[TOTAL_PARAMS] = {0};
    static int dbg_counters[TOTAL_PARAMS] = {0};
    
    while (signalk_enabled && WiFi.status() == WL_CONNECTED) {
        // Poll every 500ms for fast needle updates
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Poll all 10 paths
        for (int i = 0; i < TOTAL_PARAMS; i++) {
            if (signalk_paths[i].length() > 0) {
                char label[32];
                snprintf(label, sizeof(label), "Path[%d]: ", i);
                poll_signalk_path(signalk_paths[i], label, i, miss_counters[i], dbg_counters[i]);
            }
        }
    }
    
    Serial.println("Signal K task ended");
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
    
    // WiFi should already be connected from setup_sensESP()
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Signal K: WiFi not connected, aborting");
        signalk_enabled = false;
        return;
    }
    
    Serial.println("Signal K: Creating task...");
    Serial.flush();
    
    // Create Signal K task on core 0 with higher priority for responsive updates
    xTaskCreatePinnedToCore(
        signalk_task,
        "SignalK",
        8192,  // Increased from 5120 to prevent stack overflow
        NULL,
        3,      // Higher priority (was 0) for responsive gauge updates
        &signalk_task_handle,
        0  // Core 0
    );
    
    Serial.println("Signal K task created successfully");
    Serial.flush();
}

// Disable Signal K
void disable_signalk() {
    signalk_enabled = false;
    if (signalk_task_handle != NULL) {
        vTaskDelete(signalk_task_handle);
        signalk_task_handle = NULL;
    }
    WiFi.disconnect();
    Serial.println("Signal K disabled");
}



