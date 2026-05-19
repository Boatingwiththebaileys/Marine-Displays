#include "signalk_config.h"
#include "network_setup.h"
#include "screen_config_c_api.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include "esp_task_wdt.h"
#include <esp_heap_caps.h>
#include <map>
#include <set>
#include <vector>

extern "C" int ui_get_current_screen(void);

// STL allocator that places all nodes in PSRAM instead of iRAM.
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

// Custom ArduinoJson allocator that uses PSRAM instead of internal RAM.
// Saves ~4 KB of iRAM on every SK WebSocket message parse.
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

// Metadata storage for each parameter
String g_sensor_units[TOTAL_PARAMS];
String g_sensor_descriptions[TOTAL_PARAMS];

// Navigation globals for POSITION/COMPASS display types
volatile float g_nav_latitude  = NAN;
volatile float g_nav_longitude = NAN;
char g_nav_datetime[32]        = {0};
char g_sk_datetime[32]         = {0};  // SK writes here; RTC sync reads it

// Extended storage for paths beyond the gauge array (number displays, dual displays)
// Uses PSRAM allocator to keep map nodes out of iRAM.
static PsramMap<String, float> extended_sensor_values;
static PsramMap<String, String> extended_sensor_units;
static PsramMap<String, String> extended_sensor_descriptions;

// Path array for gauge parameter slots (loaded from NVS/SD config)
static String signalk_paths[TOTAL_PARAMS];

// Public enqueue wrapper — no-op in NMEA mode (no WebSocket)
void enqueue_signalk_message(const String &msg) { (void)msg; }

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

// Metadata getters (thread-safe)
String get_sensor_unit(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return "";
    String unit = "";
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        unit = g_sensor_units[index];
        xSemaphoreGive(sensor_mutex);
    }
    return unit;
}

String get_sensor_description(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return "";
    String desc = "";
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        desc = g_sensor_descriptions[index];
        xSemaphoreGive(sensor_mutex);
    }
    return desc;
}

void set_sensor_metadata(int index, const char* unit, const char* description) {
    if (index < 0 || index >= TOTAL_PARAMS) return;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        if (unit) g_sensor_units[index] = String(unit);
        if (description) g_sensor_descriptions[index] = String(description);
        xSemaphoreGive(sensor_mutex);
    }
}

// Get sensor value by path (for number and dual displays that may use non-gauge paths)
float get_sensor_value_by_path(const String& path) {
    if (path.length() == 0) return NAN;
    
    // First check if it's in the gauge paths array
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_value(i);
        }
    }
    
    // Check extended storage
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_values.find(path);
        float val = (it != extended_sensor_values.end()) ? it->second : NAN;
        xSemaphoreGive(sensor_mutex);
        return val;
    }
    
    return NAN;
}

// Thread-safe setter by path (for NMEA 2000 route_value)
void set_sensor_value_by_path(const String& path, float value) {
    if (path.length() == 0) return;

    // Check gauge paths first
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            set_sensor_value(i, value);
            return;
        }
    }

    // Store in extended map
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        extended_sensor_values[path] = value;
        xSemaphoreGive(sensor_mutex);
    }
}

void set_sensor_unit_by_path(const String& path, const String& unit) {
    if (path.length() == 0) return;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        extended_sensor_units[path] = unit;
        xSemaphoreGive(sensor_mutex);
    }
}

void set_sensor_description_by_path(const String& path, const String& desc) {
    if (path.length() == 0) return;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        extended_sensor_descriptions[path] = desc;
        xSemaphoreGive(sensor_mutex);
    }
}

// Get sensor unit by path
String get_sensor_unit_by_path(const String& path) {
    if (path.length() == 0) return "";
    
    // First check gauge paths
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_unit(i);
        }
    }
    
    // Check extended storage
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_units.find(path);
        String unit = (it != extended_sensor_units.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return unit;
    }
    
    return "";
}

// Get sensor description by path
String get_sensor_description_by_path(const String& path) {
    if (path.length() == 0) return "";
    
    // First check gauge paths
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_description(i);
        }
    }
    
    // Check extended storage
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_descriptions.find(path);
        String desc = (it != extended_sensor_descriptions.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return desc;
    }
    
    return "";
}

// -- Metadata stubs (no SignalK REST API in NMEA mode) --
// Metadata comes from NMEA 2000 PGN definitions, not fetched at runtime.
void fetch_all_metadata() { }

// Initialize mutex
void init_sensor_mutex() {
    if (sensor_mutex == NULL) {
        sensor_mutex = xSemaphoreCreateMutex();
    }
}

// -- SignalK WS stubs (no-ops in NMEA 2000 mode) --
// These are called from network_setup.cpp and main.cpp; kept as empty
// stubs so the rest of the codebase compiles unchanged.
void enable_signalk(const char*, const char*, const char*, uint16_t) { }
void disable_signalk() { }
bool is_signalk_ws_paused() { return false; }
void pause_signalk_ws() { }
void resume_signalk_ws() { }
void schedule_signalk_ws_resume() { }
volatile bool g_signalk_ws_resume_pending = false;

void subscribe_to_active_screen(int) { }
void refresh_signalk_subscriptions() {
    // Reload paths from config (needed so get_sensor_value_by_path works)
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
    }
}
