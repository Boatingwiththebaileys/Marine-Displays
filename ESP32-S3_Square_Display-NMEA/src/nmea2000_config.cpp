// nmea2000_config.cpp — NMEA 2000 CAN bus data source
//
// Replaces the SignalK WebSocket pipeline with direct CAN bus reception
// using the ESP32-S3 built-in TWAI peripheral + Timo Lappalainen's
// NMEA2000 library.  The same g_sensor_values[] / extended_sensor_values
// infrastructure is fed, so gauges, number displays, etc. all work
// identically — only the transport layer changes.

#include "nmea2000_config.h"
#include "signalk_config.h"     // g_sensor_values, set_sensor_value, sensor_mutex, etc.
#include "network_setup.h"      // get_signalk_path_by_index (reused for N2K field mapping)

#include "ais_display.h"            // ais_n2k_update_position/name
#include "NMEA2000_esp32_twai.h"  // Our local TWAI driver for ESP32-S3
#include <N2kMessages.h>
#include <N2kMessagesEnumToStr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// NMEA2000 instance using our TWAI driver
static tNMEA2000_twai NMEA2000_drv((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN);
tNMEA2000 &NMEA2000 = NMEA2000_drv;

// ── Runtime state ────────────────────────────────────────────────────
static TaskHandle_t n2k_task_handle = nullptr;
static bool n2k_running = false;
static bool n2k_updates_paused = false;

// ── Forward declarations ─────────────────────────────────────────────
static void n2k_task(void* param);
static void handle_n2k_msg(const tN2kMsg& msg);

// ── Human-readable labels for the web UI dropdown ────────────────────
const char* n2k_field_label(N2kField f) {
    switch (f) {
        // Engine #1
        case N2K_ENGINE_RPM:          return "Engine RPM";
        case N2K_ENGINE_BOOST:        return "Engine Boost";
        case N2K_ENGINE_TILT_TRIM:    return "Tilt / Trim";
        case N2K_OIL_PRESSURE:        return "Oil Pressure";
        case N2K_OIL_TEMPERATURE:     return "Oil Temperature";
        case N2K_COOLANT_TEMPERATURE: return "Coolant Temperature";
        case N2K_ALT_VOLTAGE:         return "Alternator Voltage";
        case N2K_FUEL_RATE:           return "Fuel Rate";
        case N2K_ENGINE_HOURS:        return "Engine Hours";
        case N2K_ENGINE_LOAD:         return "Engine Load";
        case N2K_ENGINE_TORQUE:       return "Engine Torque";
        case N2K_EXHAUST_TEMPERATURE: return "Exhaust Temperature";
        // Engine #2
        case N2K_ENGINE2_RPM:         return "Engine 2 RPM";
        case N2K_ENGINE2_OIL_PRESSURE:return "Engine 2 Oil Pressure";
        case N2K_ENGINE2_OIL_TEMP:    return "Engine 2 Oil Temp";
        case N2K_ENGINE2_COOLANT_TEMP:return "Engine 2 Coolant Temp";
        case N2K_ENGINE2_ALT_VOLTAGE: return "Engine 2 Alt Voltage";
        case N2K_ENGINE2_FUEL_RATE:   return "Engine 2 Fuel Rate";
        case N2K_ENGINE2_HOURS:       return "Engine 2 Hours";
        case N2K_ENGINE2_EXHAUST_TEMP:return "Engine 2 Exhaust Temp";
        // Transmission
        case N2K_TRANS_GEAR:          return "Transmission Gear";
        case N2K_TRANS_OIL_PRESSURE:  return "Trans Oil Pressure";
        case N2K_TRANS_OIL_TEMP:      return "Trans Oil Temp";
        // Trip
        case N2K_TRIP_FUEL_USED:      return "Trip Fuel Used";
        case N2K_FUEL_RATE_AVG:       return "Avg Fuel Rate";
        case N2K_FUEL_RATE_ECONOMY:   return "Fuel Economy";
        // Fluid levels
        case N2K_FUEL_LEVEL:          return "Fuel Level";
        case N2K_FRESHWATER_LEVEL:    return "Fresh Water Level";
        case N2K_WASTEWATER_LEVEL:    return "Waste Water Level";
        case N2K_OIL_LEVEL:           return "Oil Level";
        case N2K_BLACKWATER_LEVEL:    return "Black Water Level";
        case N2K_FUEL_CAPACITY:       return "Fuel Capacity";
        case N2K_FRESHWATER_CAPACITY: return "Fresh Water Capacity";
        case N2K_LIVEWELL_LEVEL:      return "Live Well Level";
        // Navigation
        case N2K_HEADING:             return "Heading";
        case N2K_MAGNETIC_DEVIATION:  return "Mag Deviation";
        case N2K_MAGNETIC_VARIATION_HDG: return "Mag Variation (Hdg)";
        case N2K_RATE_OF_TURN:        return "Rate of Turn";
        case N2K_MAGNETIC_VARIATION:  return "Magnetic Variation";
        case N2K_LATITUDE:            return "Latitude";
        case N2K_LONGITUDE:           return "Longitude";
        case N2K_COG:                 return "COG";
        case N2K_SOG:                 return "SOG";
        case N2K_GNSS_DATETIME:       return "GNSS Date/Time";
        case N2K_GNSS_ALTITUDE:       return "GNSS Altitude";
        case N2K_GNSS_SATELLITES:     return "GNSS Satellites";
        case N2K_GNSS_HDOP:           return "GNSS HDOP";
        case N2K_XTE:                 return "Cross Track Error";
        case N2K_WPT_DISTANCE:        return "Waypoint Distance";
        case N2K_WPT_BEARING:         return "Waypoint Bearing";
        case N2K_WPT_VMG:             return "VMG to Waypoint";
        case N2K_ETA_TIME:            return "ETA";
        case N2K_SPEED_WATER:         return "Speed (Water)";
        case N2K_SPEED_GROUND:        return "Speed (Ground)";
        case N2K_LOG_TOTAL:           return "Total Log";
        case N2K_LOG_TRIP:            return "Trip Log";
        case N2K_LEEWAY:              return "Leeway";
        // Depth
        case N2K_WATER_DEPTH:         return "Water Depth";
        case N2K_DEPTH_OFFSET:        return "Depth Offset";
        // Wind
        case N2K_WIND_SPEED_APPARENT: return "Wind Speed (Apparent)";
        case N2K_WIND_ANGLE_APPARENT: return "Wind Angle (Apparent)";
        case N2K_WIND_SPEED_TRUE:     return "Wind Speed (True)";
        case N2K_WIND_ANGLE_TRUE:     return "Wind Angle (True)";
        // Environment
        case N2K_TEMPERATURE:         return "Temperature";
        case N2K_SEA_TEMPERATURE:     return "Sea Temperature";
        case N2K_TEMPERATURE_SET:     return "Set Temperature";
        case N2K_OUTSIDE_TEMP:        return "Outside Air Temp";
        case N2K_OUTSIDE_HUMIDITY:    return "Humidity";
        case N2K_ATMOSPHERIC_PRESSURE:return "Atmospheric Pressure";
        case N2K_BAROMETRIC_PRESSURE: return "Barometric Pressure";
        // Attitude
        case N2K_PITCH:               return "Pitch";
        case N2K_ROLL:                return "Roll";
        case N2K_YAW:                 return "Yaw";
        // Rudder / Trim tabs
        case N2K_RUDDER_POSITION:     return "Rudder Position";
        case N2K_TRIM_TAB_PORT:       return "Trim Tab (Port)";
        case N2K_TRIM_TAB_STBD:       return "Trim Tab (Starboard)";
        // Battery / Electrical
        case N2K_BATTERY_VOLTAGE:     return "Battery Voltage";
        case N2K_BATTERY_CURRENT:     return "Battery Current";
        case N2K_BATTERY_TEMPERATURE: return "Battery Temperature";
        case N2K_BATTERY2_VOLTAGE:    return "Battery 2 Voltage";
        case N2K_BATTERY2_CURRENT:    return "Battery 2 Current";
        case N2K_BATTERY_SOC:         return "Battery SOC";
        case N2K_BATTERY_TIME_REMAIN: return "Battery Time Remaining";
        case N2K_DC_SOURCE_VOLTAGE:   return "DC Source Voltage";
        case N2K_DC_SOURCE_CURRENT:   return "DC Source Current";
        // Time
        case N2K_SYSTEM_TIME:         return "System Time";
        default:                      return "None";
    }
}

// SI unit strings for each N2kField — enables convert_unit() to do the
// right conversions (K→°C, Pa→bar, Hz→RPM, m/s→kn, rad→deg, etc.)
const char* n2k_field_unit(N2kField f) {
    switch (f) {
        // Engine #1
        case N2K_ENGINE_RPM:          return "Hz";
        case N2K_ENGINE_BOOST:        return "Pa";
        case N2K_ENGINE_TILT_TRIM:    return "%";
        case N2K_OIL_PRESSURE:        return "Pa";
        case N2K_OIL_TEMPERATURE:     return "K";
        case N2K_COOLANT_TEMPERATURE: return "K";
        case N2K_ALT_VOLTAGE:         return "V";
        case N2K_FUEL_RATE:           return "L/h";
        case N2K_ENGINE_HOURS:        return "s";
        case N2K_ENGINE_LOAD:         return "%";
        case N2K_ENGINE_TORQUE:       return "%";
        case N2K_EXHAUST_TEMPERATURE: return "K";
        // Engine #2 — same units as #1
        case N2K_ENGINE2_RPM:         return "Hz";
        case N2K_ENGINE2_OIL_PRESSURE:return "Pa";
        case N2K_ENGINE2_OIL_TEMP:    return "K";
        case N2K_ENGINE2_COOLANT_TEMP:return "K";
        case N2K_ENGINE2_ALT_VOLTAGE: return "V";
        case N2K_ENGINE2_FUEL_RATE:   return "L/h";
        case N2K_ENGINE2_HOURS:       return "s";
        case N2K_ENGINE2_EXHAUST_TEMP:return "K";
        // Transmission
        case N2K_TRANS_GEAR:          return "";
        case N2K_TRANS_OIL_PRESSURE:  return "Pa";
        case N2K_TRANS_OIL_TEMP:      return "K";
        // Trip
        case N2K_TRIP_FUEL_USED:      return "L/h";  // library gives litres
        case N2K_FUEL_RATE_AVG:       return "L/h";
        case N2K_FUEL_RATE_ECONOMY:   return "L/h";
        // Fluid levels
        case N2K_FUEL_LEVEL:          return "ratio";
        case N2K_FRESHWATER_LEVEL:    return "ratio";
        case N2K_WASTEWATER_LEVEL:    return "ratio";
        case N2K_OIL_LEVEL:           return "ratio";
        case N2K_BLACKWATER_LEVEL:    return "ratio";
        case N2K_FUEL_CAPACITY:       return "m3";
        case N2K_FRESHWATER_CAPACITY: return "m3";
        case N2K_LIVEWELL_LEVEL:      return "ratio";
        // Navigation
        case N2K_HEADING:             return "";   // already degrees
        case N2K_MAGNETIC_DEVIATION:  return "rad";
        case N2K_MAGNETIC_VARIATION_HDG: return "rad";
        case N2K_RATE_OF_TURN:        return "rad";  // rad/s
        case N2K_MAGNETIC_VARIATION:  return "rad";
        case N2K_LATITUDE:            return "";
        case N2K_LONGITUDE:           return "";
        case N2K_COG:                 return "";   // already degrees
        case N2K_SOG:                 return "m/s";
        case N2K_GNSS_DATETIME:       return "";
        case N2K_GNSS_ALTITUDE:       return "m_depth";
        case N2K_GNSS_SATELLITES:     return "";
        case N2K_GNSS_HDOP:           return "";
        case N2K_XTE:                 return "m_depth";
        case N2K_WPT_DISTANCE:        return "m";
        case N2K_WPT_BEARING:         return "";   // already degrees
        case N2K_WPT_VMG:             return "m/s";
        case N2K_ETA_TIME:            return "";
        case N2K_SPEED_WATER:         return "m/s";
        case N2K_SPEED_GROUND:        return "m/s";
        case N2K_LOG_TOTAL:           return "m";
        case N2K_LOG_TRIP:            return "m";
        case N2K_LEEWAY:              return "rad";
        // Depth
        case N2K_WATER_DEPTH:         return "m_depth";
        case N2K_DEPTH_OFFSET:        return "m_depth";
        // Wind
        case N2K_WIND_SPEED_APPARENT: return "m/s";
        case N2K_WIND_ANGLE_APPARENT: return "";
        case N2K_WIND_SPEED_TRUE:     return "m/s";
        case N2K_WIND_ANGLE_TRUE:     return "";
        // Environment
        case N2K_TEMPERATURE:         return "K";
        case N2K_SEA_TEMPERATURE:     return "K";
        case N2K_TEMPERATURE_SET:     return "K";
        case N2K_OUTSIDE_TEMP:        return "K";
        case N2K_OUTSIDE_HUMIDITY:    return "ratio";
        case N2K_ATMOSPHERIC_PRESSURE:return "Pa";
        case N2K_BAROMETRIC_PRESSURE: return "Pa";
        // Attitude
        case N2K_PITCH:               return "rad";
        case N2K_ROLL:                return "rad";
        case N2K_YAW:                 return "rad";
        // Rudder / Trim tabs
        case N2K_RUDDER_POSITION:     return "rad";
        case N2K_TRIM_TAB_PORT:       return "";
        case N2K_TRIM_TAB_STBD:       return "";
        // Battery / Electrical
        case N2K_BATTERY_VOLTAGE:     return "V";
        case N2K_BATTERY_CURRENT:     return "A";
        case N2K_BATTERY_TEMPERATURE: return "K";
        case N2K_BATTERY2_VOLTAGE:    return "V";
        case N2K_BATTERY2_CURRENT:    return "A";
        case N2K_BATTERY_SOC:         return "ratio";
        case N2K_BATTERY_TIME_REMAIN: return "s";
        case N2K_DC_SOURCE_VOLTAGE:   return "V";
        case N2K_DC_SOURCE_CURRENT:   return "A";
        // Time
        case N2K_SYSTEM_TIME:         return "";
        default:                      return "";
    }
}

// Map SignalK-style path strings to N2kField values.
// This lets existing screen_configs (which store SK paths) work on the
// NMEA branch — the path is looked up here to find which CAN PGN field
// to route to that gauge / number display slot.
N2kField n2k_field_from_path(const String& path) {
    if (path.length() == 0) return N2K_NONE;
    // Engine #1
    if (path.indexOf("revolutions") >= 0)               return N2K_ENGINE_RPM;
    if (path.indexOf("boostPressure") >= 0)              return N2K_ENGINE_BOOST;
    if (path.indexOf("oilPressure") >= 0)                return N2K_OIL_PRESSURE;
    if (path.indexOf("oilTemperature") >= 0)             return N2K_OIL_TEMPERATURE;
    if (path.indexOf("coolantTemperature") >= 0 ||
        path.indexOf("coolantTemp") >= 0)                return N2K_COOLANT_TEMPERATURE;
    if (path.indexOf("alternatorVoltage") >= 0)          return N2K_ALT_VOLTAGE;
    if (path.indexOf("fuel.rate") >= 0 ||
        path.indexOf("fuelRate") >= 0)                   return N2K_FUEL_RATE;
    if (path.indexOf("runTime") >= 0 ||
        path.indexOf("engineHours") >= 0)                return N2K_ENGINE_HOURS;
    if (path.indexOf("engineLoad") >= 0 ||
        path.indexOf("load") >= 0)                       return N2K_ENGINE_LOAD;
    if (path.indexOf("engineTorque") >= 0 ||
        path.indexOf("torque") >= 0)                     return N2K_ENGINE_TORQUE;
    if (path.indexOf("exhaustTemperature") >= 0 ||
        path.indexOf("exhaustTemp") >= 0)                return N2K_EXHAUST_TEMPERATURE;
    if (path.indexOf("tiltTrim") >= 0 ||
        path.indexOf("drive.trimState") >= 0)            return N2K_ENGINE_TILT_TRIM;
    // Fluid levels
    if (path.indexOf("fuel") >= 0 &&
        path.indexOf("currentLevel") >= 0)               return N2K_FUEL_LEVEL;
    if (path.indexOf("freshWater") >= 0 &&
        path.indexOf("currentLevel") >= 0)               return N2K_FRESHWATER_LEVEL;
    if (path.indexOf("wasteWater") >= 0)                 return N2K_WASTEWATER_LEVEL;
    if (path.indexOf("blackWater") >= 0)                 return N2K_BLACKWATER_LEVEL;
    if (path.indexOf("liveWell") >= 0)                   return N2K_LIVEWELL_LEVEL;
    if (path.indexOf("fuel") >= 0 &&
        path.indexOf("capacity") >= 0)                   return N2K_FUEL_CAPACITY;
    if (path.indexOf("freshWater") >= 0 &&
        path.indexOf("capacity") >= 0)                   return N2K_FRESHWATER_CAPACITY;
    // Navigation
    if (path.indexOf("headingMagnetic") >= 0 ||
        path.indexOf("headingTrue") >= 0 ||
        path.indexOf("navigation.heading") >= 0)         return N2K_HEADING;
    if (path.indexOf("rateOfTurn") >= 0)                 return N2K_RATE_OF_TURN;
    if (path.indexOf("magneticVariation") >= 0)          return N2K_MAGNETIC_VARIATION;
    if (path.indexOf("navigation.position") >= 0)        return N2K_LATITUDE;
    if (path.indexOf("courseOverGround") >= 0)            return N2K_COG;
    if (path.indexOf("speedOverGround") >= 0)            return N2K_SOG;
    if (path.indexOf("crossTrackError") >= 0 ||
        path.indexOf("xte") >= 0)                        return N2K_XTE;
    if (path.indexOf("speedThroughWater") >= 0)          return N2K_SPEED_WATER;
    if (path.indexOf("log") >= 0 &&
        path.indexOf("trip") >= 0)                       return N2K_LOG_TRIP;
    if (path.indexOf("log") >= 0)                        return N2K_LOG_TOTAL;
    if (path.indexOf("leewayAngle") >= 0)                return N2K_LEEWAY;
    if (path.indexOf("depth") >= 0 &&
        path.indexOf("offset") >= 0)                     return N2K_DEPTH_OFFSET;
    if (path.indexOf("depth") >= 0)                      return N2K_WATER_DEPTH;
    // Wind
    if (path.indexOf("wind") >= 0 &&
        path.indexOf("speedTrue") >= 0)                  return N2K_WIND_SPEED_TRUE;
    if (path.indexOf("wind") >= 0 &&
        path.indexOf("angleTrue") >= 0)                  return N2K_WIND_ANGLE_TRUE;
    if (path.indexOf("wind.speed") >= 0 ||
        path.indexOf("windSpeed") >= 0)                  return N2K_WIND_SPEED_APPARENT;
    if (path.indexOf("wind.angle") >= 0 ||
        path.indexOf("windAngle") >= 0)                  return N2K_WIND_ANGLE_APPARENT;
    // Environment
    if (path.indexOf("seaTemperature") >= 0)             return N2K_SEA_TEMPERATURE;
    if (path.indexOf("outsideTemperature") >= 0)         return N2K_OUTSIDE_TEMP;
    if (path.indexOf("humidity") >= 0)                   return N2K_OUTSIDE_HUMIDITY;
    if (path.indexOf("atmosphericPressure") >= 0 ||
        path.indexOf("barometric") >= 0)                 return N2K_ATMOSPHERIC_PRESSURE;
    if (path.indexOf("temperature") >= 0)                return N2K_TEMPERATURE;
    // Attitude
    if (path.indexOf("pitch") >= 0)                      return N2K_PITCH;
    if (path.indexOf("roll") >= 0)                       return N2K_ROLL;
    if (path.indexOf("yaw") >= 0)                        return N2K_YAW;
    // Rudder
    if (path.indexOf("rudder") >= 0)                     return N2K_RUDDER_POSITION;
    // Battery
    if (path.indexOf("battery") >= 0 &&
        path.indexOf("voltage") >= 0)                    return N2K_BATTERY_VOLTAGE;
    if (path.indexOf("battery") >= 0 &&
        path.indexOf("current") >= 0)                    return N2K_BATTERY_CURRENT;
    if (path.indexOf("stateOfCharge") >= 0)              return N2K_BATTERY_SOC;
    // Datetime
    if (path.indexOf("datetime") >= 0)                   return N2K_GNSS_DATETIME;

    return N2K_NONE;
}

// Map a stored config value to an N2kField.
// New configs store the enum as a decimal string (e.g. "1", "30").
// Legacy configs store an SK path string — fall back to n2k_field_from_path().
N2kField n2k_field_from_stored(const String& stored) {
    if (stored.length() == 0) return N2K_NONE;
    // If the stored value is purely numeric, treat as enum
    bool allDigit = true;
    for (unsigned i = 0; i < stored.length(); i++) {
        if (!isDigit(stored[i])) { allDigit = false; break; }
    }
    if (allDigit) {
        int v = stored.toInt();
        if (v > 0 && v < (int)N2K_FIELD_COUNT) return (N2kField)v;
        return N2K_NONE;
    }
    return n2k_field_from_path(stored);
}

// ── Route a decoded value to every gauge/number slot that wants this field ──
static void route_value(N2kField field, float value) {
    // Check the 10 gauge parameter slots
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        String path = get_signalk_path_by_index(i);
        if (n2k_field_from_stored(path) == field) {
            set_sensor_value(i, value);
        }
    }
    // Store in the extended map keyed by the numeric enum string so
    // screen_configs lookups (which store the enum as a string) find it.
    String key = String((int)field);
    set_sensor_value_by_path(key, value);
    // Set SI unit + description once so convert_unit works on displays
    const char* unit = n2k_field_unit(field);
    if (unit[0]) {
        set_sensor_unit_by_path(key, String(unit));
    }
    set_sensor_description_by_path(key, String(n2k_field_label(field)));
}

// ── PGN Handlers ─────────────────────────────────────────────────────

static void handle_engine_rapid(const tN2kMsg& msg) {          // PGN 127488
    unsigned char instance;
    double rpm, boost;
    int8_t tilt;
    if (ParseN2kEngineParamRapid(msg, instance, rpm, boost, tilt)) {
        bool eng2 = (instance == 1);
        if (!N2kIsNA(rpm))   route_value(eng2 ? N2K_ENGINE2_RPM : N2K_ENGINE_RPM, (float)rpm);
        if (!N2kIsNA(boost)) route_value(N2K_ENGINE_BOOST, (float)boost);
        if (tilt != N2kInt8NA) route_value(N2K_ENGINE_TILT_TRIM, (float)tilt);
    }
}

static void handle_engine_dynamic(const tN2kMsg& msg) {        // PGN 127489
    unsigned char instance;
    double oilP, oilT, coolT, altV, fuelR, hours, coolP, fuelP;
    int8_t load, torque;
    tN2kEngineDiscreteStatus1 st1;
    tN2kEngineDiscreteStatus2 st2;
    if (ParseN2kEngineDynamicParam(msg, instance,
            oilP, oilT, coolT, altV, fuelR, hours,
            coolP, fuelP, load, torque, st1, st2)) {
        bool eng2 = (instance == 1);
        if (!N2kIsNA(oilP))  route_value(eng2 ? N2K_ENGINE2_OIL_PRESSURE : N2K_OIL_PRESSURE, (float)oilP);
        if (!N2kIsNA(oilT))  route_value(eng2 ? N2K_ENGINE2_OIL_TEMP : N2K_OIL_TEMPERATURE, (float)oilT);
        if (!N2kIsNA(coolT)) route_value(eng2 ? N2K_ENGINE2_COOLANT_TEMP : N2K_COOLANT_TEMPERATURE, (float)coolT);
        if (!N2kIsNA(altV))  route_value(eng2 ? N2K_ENGINE2_ALT_VOLTAGE : N2K_ALT_VOLTAGE, (float)altV);
        if (!N2kIsNA(fuelR)) route_value(eng2 ? N2K_ENGINE2_FUEL_RATE : N2K_FUEL_RATE, (float)fuelR);
        if (!N2kIsNA(hours)) route_value(eng2 ? N2K_ENGINE2_HOURS : N2K_ENGINE_HOURS, (float)hours);
        if (load != N2kInt8NA) route_value(N2K_ENGINE_LOAD, (float)load);
        if (torque != N2kInt8NA) route_value(N2K_ENGINE_TORQUE, (float)torque);
    }
}

static void handle_transmission(const tN2kMsg& msg) {          // PGN 127493
    unsigned char instance;
    tN2kTransmissionGear gear;
    double oilP, oilT;
    unsigned char discrete;
    if (ParseN2kTransmissionParameters(msg, instance, gear, oilP, oilT, discrete)) {
        route_value(N2K_TRANS_GEAR, (float)gear);
        if (!N2kIsNA(oilP)) route_value(N2K_TRANS_OIL_PRESSURE, (float)oilP);
        if (!N2kIsNA(oilT)) route_value(N2K_TRANS_OIL_TEMP, (float)oilT);
    }
}

static void handle_engine_trip(const tN2kMsg& msg) {            // PGN 127497
    unsigned char instance;
    double tripFuel, avgRate, ecoRate, instantEco;
    if (ParseN2kEngineTripParameters(msg, instance, tripFuel, avgRate, ecoRate, instantEco)) {
        if (!N2kIsNA(tripFuel)) route_value(N2K_TRIP_FUEL_USED, (float)tripFuel);
        if (!N2kIsNA(avgRate))  route_value(N2K_FUEL_RATE_AVG, (float)avgRate);
        if (!N2kIsNA(ecoRate))  route_value(N2K_FUEL_RATE_ECONOMY, (float)ecoRate);
    }
}

static void handle_fluid_level(const tN2kMsg& msg) {           // PGN 127505
    unsigned char instance;
    tN2kFluidType type;
    double level, capacity;
    if (ParseN2kFluidLevel(msg, instance, type, level, capacity)) {
        if (!N2kIsNA(level)) {
            switch (type) {
                case N2kft_Fuel:       route_value(N2K_FUEL_LEVEL, (float)level); break;
                case N2kft_Water:      route_value(N2K_FRESHWATER_LEVEL, (float)level); break;
                case N2kft_GrayWater:  route_value(N2K_WASTEWATER_LEVEL, (float)level); break;
                case N2kft_BlackWater: route_value(N2K_BLACKWATER_LEVEL, (float)level); break;
                case N2kft_LiveWell:   route_value(N2K_LIVEWELL_LEVEL, (float)level); break;
                case N2kft_Oil:        route_value(N2K_OIL_LEVEL, (float)level); break;
                default: break;
            }
        }
        if (!N2kIsNA(capacity)) {
            if (type == N2kft_Fuel) route_value(N2K_FUEL_CAPACITY, (float)capacity);
            if (type == N2kft_Water) route_value(N2K_FRESHWATER_CAPACITY, (float)capacity);
        }
    }
}

static void handle_rudder(const tN2kMsg& msg) {                // PGN 127245
    double pos;
    if (ParseN2kRudder(msg, pos)) {
        if (!N2kIsNA(pos)) route_value(N2K_RUDDER_POSITION, (float)pos);
    }
}

static void handle_heading(const tN2kMsg& msg) {               // PGN 127250
    unsigned char sid;
    double heading, deviation, variation;
    tN2kHeadingReference ref;
    if (ParseN2kHeading(msg, sid, heading, deviation, variation, ref)) {
        if (!N2kIsNA(heading))   route_value(N2K_HEADING, (float)(heading * 180.0 / PI));
        if (!N2kIsNA(deviation)) route_value(N2K_MAGNETIC_DEVIATION, (float)deviation);
        if (!N2kIsNA(variation)) route_value(N2K_MAGNETIC_VARIATION_HDG, (float)variation);
    }
}

static void handle_rate_of_turn(const tN2kMsg& msg) {          // PGN 127251
    unsigned char sid;
    double rot;
    if (ParseN2kRateOfTurn(msg, sid, rot)) {
        if (!N2kIsNA(rot)) route_value(N2K_RATE_OF_TURN, (float)rot);
    }
}

static void handle_attitude(const tN2kMsg& msg) {              // PGN 127257
    unsigned char sid;
    double yaw, pitch, roll;
    if (ParseN2kAttitude(msg, sid, yaw, pitch, roll)) {
        if (!N2kIsNA(yaw))   route_value(N2K_YAW,   (float)yaw);
        if (!N2kIsNA(pitch)) route_value(N2K_PITCH,  (float)pitch);
        if (!N2kIsNA(roll))  route_value(N2K_ROLL,   (float)roll);
    }
}

static void handle_magnetic_variation(const tN2kMsg& msg) {    // PGN 127258
    unsigned char sid;
    tN2kMagneticVariation source;
    uint16_t days;
    double variation;
    if (ParseN2kMagneticVariation(msg, sid, source, days, variation)) {
        if (!N2kIsNA(variation)) route_value(N2K_MAGNETIC_VARIATION, (float)variation);
    }
}

static void handle_dc_status(const tN2kMsg& msg) {             // PGN 127506
    unsigned char sid, dcInst, soc, soh;
    tN2kDCType dcType;
    double timeRemain, ripple, capacity;
    if (ParseN2kDCStatus(msg, sid, dcInst, dcType, soc, soh, timeRemain, ripple, capacity)) {
        if (soc != 0xFF)           route_value(N2K_BATTERY_SOC, (float)soc / 100.0f);
        if (!N2kIsNA(timeRemain))  route_value(N2K_BATTERY_TIME_REMAIN, (float)timeRemain);
    }
}

static void handle_battery(const tN2kMsg& msg) {               // PGN 127508
    unsigned char instance;
    double voltage, current, temperature;
    unsigned char sid;
    if (ParseN2kDCBatStatus(msg, instance, voltage, current, temperature, sid)) {
        bool bank2 = (instance == 1);
        if (!N2kIsNA(voltage))     route_value(bank2 ? N2K_BATTERY2_VOLTAGE : N2K_BATTERY_VOLTAGE, (float)voltage);
        if (!N2kIsNA(current))     route_value(bank2 ? N2K_BATTERY2_CURRENT : N2K_BATTERY_CURRENT, (float)current);
        if (!N2kIsNA(temperature)) route_value(N2K_BATTERY_TEMPERATURE, (float)temperature);
    }
}

static void handle_dc_voltage_current(const tN2kMsg& msg) {    // PGN 127751
    unsigned char instance, sid;
    double voltage, current;
    if (ParseN2kDCVoltageCurrent(msg, instance, voltage, current, sid)) {
        if (!N2kIsNA(voltage)) route_value(N2K_DC_SOURCE_VOLTAGE, (float)voltage);
        if (!N2kIsNA(current)) route_value(N2K_DC_SOURCE_CURRENT, (float)current);
    }
}

static void handle_leeway(const tN2kMsg& msg) {                // PGN 128000
    unsigned char sid;
    double leeway;
    if (ParseN2kLeeway(msg, sid, leeway)) {
        if (!N2kIsNA(leeway)) route_value(N2K_LEEWAY, (float)leeway);
    }
}

static void handle_boat_speed(const tN2kMsg& msg) {            // PGN 128259
    unsigned char sid;
    double water, ground;
    tN2kSpeedWaterReferenceType swrt;
    if (ParseN2kBoatSpeed(msg, sid, water, ground, swrt)) {
        if (!N2kIsNA(water))  route_value(N2K_SPEED_WATER, (float)water);
        if (!N2kIsNA(ground)) route_value(N2K_SPEED_GROUND, (float)ground);
    }
}

static void handle_water_depth(const tN2kMsg& msg) {           // PGN 128267
    unsigned char sid;
    double depth, offset;
    if (ParseN2kWaterDepth(msg, sid, depth, offset)) {
        if (!N2kIsNA(depth))  route_value(N2K_WATER_DEPTH, (float)depth);
        if (!N2kIsNA(offset)) route_value(N2K_DEPTH_OFFSET, (float)offset);
    }
}

static void handle_distance_log(const tN2kMsg& msg) {          // PGN 128275
    uint16_t days;
    double secs;
    uint32_t log, tripLog;
    if (ParseN2kDistanceLog(msg, days, secs, log, tripLog)) {
        if (log != 0xFFFFFFFF)     route_value(N2K_LOG_TOTAL, (float)log);
        if (tripLog != 0xFFFFFFFF) route_value(N2K_LOG_TRIP, (float)tripLog);
    }
}

static void handle_position_rapid(const tN2kMsg& msg) {        // PGN 129025
    double lat, lon;
    if (ParseN2kPositionRapid(msg, lat, lon)) {
        if (!N2kIsNA(lat)) {
            g_nav_latitude  = (float)lat;
            route_value(N2K_LATITUDE, (float)lat);
        }
        if (!N2kIsNA(lon)) {
            g_nav_longitude = (float)lon;
            route_value(N2K_LONGITUDE, (float)lon);
        }
    }
}

static void handle_cog_sog(const tN2kMsg& msg) {               // PGN 129026
    unsigned char sid;
    tN2kHeadingReference ref;
    double cog, sog;
    if (ParseN2kCOGSOGRapid(msg, sid, ref, cog, sog)) {
        if (!N2kIsNA(cog)) route_value(N2K_COG, (float)(cog * 180.0 / PI));
        if (!N2kIsNA(sog)) route_value(N2K_SOG, (float)sog);
    }
}

static void handle_gnss(const tN2kMsg& msg) {                  // PGN 129029
    unsigned char sid;
    uint16_t daysSince1970;
    double secondsSinceMidnight;
    double lat, lon, alt;
    tN2kGNSStype gnssType;
    tN2kGNSSmethod gnssMethod;
    unsigned char nSatellites;
    double hdop, pdop;
    double geoidalSep;
    unsigned char nReferenceStations;
    tN2kGNSStype refStationType;
    uint16_t refStationID;
    double refStationAge;
    if (ParseN2kGNSS(msg, sid, daysSince1970, secondsSinceMidnight,
                      lat, lon, alt, gnssType, gnssMethod,
                      nSatellites, hdop, pdop, geoidalSep,
                      nReferenceStations, refStationType,
                      refStationID, refStationAge)) {
        if (daysSince1970 > 0) {
            time_t epoch = (time_t)daysSince1970 * 86400 + (time_t)secondsSinceMidnight;
            struct tm t;
            gmtime_r(&epoch, &t);
            snprintf(g_sk_datetime, sizeof(g_sk_datetime),
                     "%04d-%02d-%02dT%02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        }
        if (!N2kIsNA(lat)) g_nav_latitude  = (float)lat;
        if (!N2kIsNA(lon)) g_nav_longitude = (float)lon;
        if (!N2kIsNA(alt)) route_value(N2K_GNSS_ALTITUDE, (float)alt);
        route_value(N2K_GNSS_SATELLITES, (float)nSatellites);
        if (!N2kIsNA(hdop)) route_value(N2K_GNSS_HDOP, (float)hdop);
    }
}

static void handle_xte(const tN2kMsg& msg) {                   // PGN 129283
    unsigned char sid;
    tN2kXTEMode mode;
    bool navTerminated;
    double xte;
    if (ParseN2kXTE(msg, sid, mode, navTerminated, xte)) {
        if (!N2kIsNA(xte)) route_value(N2K_XTE, (float)xte);
    }
}

static void handle_navigation_info(const tN2kMsg& msg) {       // PGN 129284
    unsigned char sid;
    double distToWpt, etaTime, bearOrig, bearPos;
    tN2kHeadingReference bearRef;
    bool perpCrossed, arrivalEntered;
    tN2kDistanceCalculationType calcType;
    int16_t etaDate;
    uint32_t origWpt, destWpt;
    double wptLat, wptLon, vmg;
    if (ParseN2kNavigationInfo(msg, sid, distToWpt, bearRef,
            perpCrossed, arrivalEntered, calcType,
            etaTime, etaDate, bearOrig, bearPos,
            origWpt, destWpt, wptLat, wptLon, vmg)) {
        if (!N2kIsNA(distToWpt)) route_value(N2K_WPT_DISTANCE, (float)distToWpt);
        if (!N2kIsNA(bearPos))   route_value(N2K_WPT_BEARING, (float)(bearPos * 180.0 / PI));
        if (!N2kIsNA(vmg))       route_value(N2K_WPT_VMG, (float)vmg);
    }
}

static void handle_wind(const tN2kMsg& msg) {                  // PGN 130306
    unsigned char sid;
    double speed, angle;
    tN2kWindReference ref;
    if (ParseN2kWindSpeed(msg, sid, speed, angle, ref)) {
        if (ref == N2kWind_Apparent) {
            if (!N2kIsNA(speed)) route_value(N2K_WIND_SPEED_APPARENT, (float)speed);
            if (!N2kIsNA(angle)) route_value(N2K_WIND_ANGLE_APPARENT, (float)(angle * 180.0 / PI));
        } else {
            // True wind (ground-referenced or water-referenced)
            if (!N2kIsNA(speed)) route_value(N2K_WIND_SPEED_TRUE, (float)speed);
            if (!N2kIsNA(angle)) route_value(N2K_WIND_ANGLE_TRUE, (float)(angle * 180.0 / PI));
        }
    }
}

static void handle_outside_env(const tN2kMsg& msg) {           // PGN 130310
    unsigned char sid;
    double waterTemp, airTemp, pressure;
    if (ParseN2kOutsideEnvironmentalParameters(msg, sid, waterTemp, airTemp, pressure)) {
        if (!N2kIsNA(waterTemp)) route_value(N2K_SEA_TEMPERATURE, (float)waterTemp);
        if (!N2kIsNA(airTemp))   route_value(N2K_OUTSIDE_TEMP, (float)airTemp);
        if (!N2kIsNA(pressure))  route_value(N2K_ATMOSPHERIC_PRESSURE, (float)pressure);
    }
}

static void handle_env_params(const tN2kMsg& msg) {            // PGN 130311
    unsigned char sid;
    tN2kTempSource tempSrc;
    double temp;
    tN2kHumiditySource humSrc;
    double humidity, pressure;
    if (ParseN2kEnvironmentalParameters(msg, sid, tempSrc, temp, humSrc, humidity, pressure)) {
        if (!N2kIsNA(temp))     route_value(N2K_TEMPERATURE, (float)temp);
        if (!N2kIsNA(humidity)) route_value(N2K_OUTSIDE_HUMIDITY, (float)(humidity / 100.0));
        if (!N2kIsNA(pressure)) route_value(N2K_BAROMETRIC_PRESSURE, (float)pressure);
    }
}

static void handle_temperature(const tN2kMsg& msg) {           // PGN 130312
    unsigned char sid, instance;
    tN2kTempSource source;
    double actual, setTemp;
    if (ParseN2kTemperature(msg, sid, instance, source, actual, setTemp)) {
        if (!N2kIsNA(actual)) {
            if (source == N2kts_ExhaustGasTemperature)
                route_value(N2K_EXHAUST_TEMPERATURE, (float)actual);
            else if (source == N2kts_SeaTemperature)
                route_value(N2K_SEA_TEMPERATURE, (float)actual);
            else
                route_value(N2K_TEMPERATURE, (float)actual);
        }
        if (!N2kIsNA(setTemp)) route_value(N2K_TEMPERATURE_SET, (float)setTemp);
    }
}

static void handle_pressure(const tN2kMsg& msg) {              // PGN 130314
    unsigned char sid, instance;
    tN2kPressureSource source;
    double pressure;
    if (ParseN2kPressure(msg, sid, instance, source, pressure)) {
        if (!N2kIsNA(pressure)) {
            if (source == N2kps_Atmospheric)
                route_value(N2K_ATMOSPHERIC_PRESSURE, (float)pressure);
            else
                route_value(N2K_BAROMETRIC_PRESSURE, (float)pressure);
        }
    }
}

static void handle_trim_tab(const tN2kMsg& msg) {              // PGN 130576
    int8_t port, stbd;
    if (ParseN2kTrimTab(msg, port, stbd)) {
        if (port != N2kInt8NA) route_value(N2K_TRIM_TAB_PORT, (float)port);
        if (stbd != N2kInt8NA) route_value(N2K_TRIM_TAB_STBD, (float)stbd);
    }
}

static void handle_system_time(const tN2kMsg& msg) {           // PGN 126992
    unsigned char sid;
    uint16_t days;
    double secs;
    tN2kTimeSource source;
    if (ParseN2kSystemTime(msg, sid, days, secs, source)) {
        if (days > 0) {
            time_t epoch = (time_t)days * 86400 + (time_t)secs;
            struct tm t;
            gmtime_r(&epoch, &t);
            snprintf(g_sk_datetime, sizeof(g_sk_datetime),
                     "%04d-%02d-%02dT%02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
        }
    }
}

// ── AIS PGN handlers ─────────────────────────────────────────────────
static void handle_ais_class_a_pos(const tN2kMsg& msg) {       // PGN 129038
    uint8_t messageID, seconds, sid;
    tN2kAISRepeat repeat;
    uint32_t userID;
    double lat, lon, cog, sog, heading, rot;
    bool accuracy, raim;
    tN2kAISNavStatus navStatus;
    tN2kAISTransceiverInformation aisInfo;
    if (ParseN2kPGN129038(msg, messageID, repeat, userID,
                          lat, lon, accuracy, raim, seconds,
                          cog, sog, heading, rot, navStatus,
                          aisInfo, sid)) {
        double cog_deg = !N2kIsNA(cog)  ? RadToDeg(cog)  : 0.0;
        double sog_kn  = !N2kIsNA(sog)  ? sog * 3600.0 / 1852.0 : 0.0;  // m/s → knots
        if (!N2kIsNA(lat) && !N2kIsNA(lon))
            ais_n2k_update_position(userID, lat, lon, cog_deg, sog_kn);
    }
}

static void handle_ais_class_b_pos(const tN2kMsg& msg) {       // PGN 129039
    uint8_t messageID, seconds, sid;
    tN2kAISRepeat repeat;
    uint32_t userID;
    double lat, lon, cog, sog, heading;
    bool accuracy, raim, display, dsc, band, msg22, state;
    tN2kAISUnit unit;
    tN2kAISMode mode;
    tN2kAISTransceiverInformation aisInfo;
    if (ParseN2kPGN129039(msg, messageID, repeat, userID,
                          lat, lon, accuracy, raim, seconds,
                          cog, sog, aisInfo, heading,
                          unit, display, dsc, band, msg22, mode, state, sid)) {
        double cog_deg = !N2kIsNA(cog)  ? RadToDeg(cog)  : 0.0;
        double sog_kn  = !N2kIsNA(sog)  ? sog * 3600.0 / 1852.0 : 0.0;
        if (!N2kIsNA(lat) && !N2kIsNA(lon))
            ais_n2k_update_position(userID, lat, lon, cog_deg, sog_kn);
    }
}

static void handle_ais_class_a_static(const tN2kMsg& msg) {    // PGN 129794
    uint8_t messageID, vesselType, sid;
    tN2kAISRepeat repeat;
    uint32_t userID, imoNumber;
    char callsign[8], name[21], destination[21];
    double length, beam, posRefStbd, posRefBow, draught, etaTime;
    uint16_t etaDate;
    tN2kAISVersion aisVersion;
    tN2kGNSStype gnssType;
    tN2kAISDTE dte;
    tN2kAISTransceiverInformation aisInfo;
    if (ParseN2kPGN129794(msg, messageID, repeat, userID,
                          imoNumber, callsign, sizeof(callsign),
                          name, sizeof(name), vesselType,
                          length, beam, posRefStbd, posRefBow,
                          etaDate, etaTime, draught,
                          destination, sizeof(destination),
                          aisVersion, gnssType, dte, aisInfo, sid)) {
        ais_n2k_update_name(userID, name);
    }
}

static void handle_ais_class_b_static_a(const tN2kMsg& msg) {  // PGN 129809
    uint8_t messageID, sid;
    tN2kAISRepeat repeat;
    uint32_t userID;
    char name[21];
    tN2kAISTransceiverInformation aisInfo;
    if (ParseN2kPGN129809(msg, messageID, repeat, userID,
                          name, sizeof(name), aisInfo, sid)) {
        ais_n2k_update_name(userID, name);
    }
}

static void handle_ais_class_b_static_b(const tN2kMsg& msg) {  // PGN 129810
    uint8_t messageID, vesselType, sid;
    tN2kAISRepeat repeat;
    uint32_t userID, mothershipID;
    char vendor[8], callsign[8];
    double length, beam, posRefStbd, posRefBow;
    tN2kAISTransceiverInformation aisInfo;
    if (ParseN2kPGN129810(msg, messageID, repeat, userID,
                          vesselType, vendor, sizeof(vendor),
                          callsign, sizeof(callsign),
                          length, beam, posRefStbd, posRefBow,
                          mothershipID, aisInfo, sid)) {
        // Class B Part B carries callsign but no name — nothing to update here
        // unless we want to store callsign in name as fallback
        if (callsign[0]) ais_n2k_update_name(userID, callsign);
    }
}

// ── Dispatch incoming NMEA 2000 messages ─────────────────────────────
static void handle_n2k_msg(const tN2kMsg& msg) {
    switch (msg.PGN) {
        case 126992: handle_system_time(msg);       break;
        case 127245: handle_rudder(msg);            break;
        case 127250: handle_heading(msg);           break;
        case 127251: handle_rate_of_turn(msg);      break;
        case 127257: handle_attitude(msg);          break;
        case 127258: handle_magnetic_variation(msg); break;
        case 127488: handle_engine_rapid(msg);      break;
        case 127489: handle_engine_dynamic(msg);    break;
        case 127493: handle_transmission(msg);      break;
        case 127497: handle_engine_trip(msg);       break;
        case 127505: handle_fluid_level(msg);       break;
        case 127506: handle_dc_status(msg);         break;
        case 127508: handle_battery(msg);           break;
        case 127751: handle_dc_voltage_current(msg); break;
        case 128000: handle_leeway(msg);            break;
        case 128259: handle_boat_speed(msg);        break;
        case 128267: handle_water_depth(msg);       break;
        case 128275: handle_distance_log(msg);      break;
        case 129025: handle_position_rapid(msg);    break;
        case 129026: handle_cog_sog(msg);           break;
        case 129029: handle_gnss(msg);              break;
        case 129038: handle_ais_class_a_pos(msg);   break;
        case 129039: handle_ais_class_b_pos(msg);   break;
        case 129283: handle_xte(msg);               break;
        case 129284: handle_navigation_info(msg);   break;
        case 129794: handle_ais_class_a_static(msg); break;
        case 129809: handle_ais_class_b_static_a(msg); break;
        case 129810: handle_ais_class_b_static_b(msg); break;
        case 130306: handle_wind(msg);              break;
        case 130310: handle_outside_env(msg);       break;
        case 130311: handle_env_params(msg);        break;
        case 130312: handle_temperature(msg);       break;
        case 130314: handle_pressure(msg);          break;
        case 130576: handle_trim_tab(msg);          break;
        default: break;
    }
}

// ── FreeRTOS task — runs on core 0 to keep TWAI ISR off the LCD core ─
static void n2k_task(void* param) {
    // Open CAN bus HERE on core 0 so the TWAI interrupt is allocated
    // on core 0, avoiding conflicts with the LCD GDMA ISR on core 1.
    if (!NMEA2000.Open()) {
        Serial.println("[N2K] ERROR: CAN bus failed to open");
        n2k_running = false;
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[N2K] CAN bus opened (TX=%d, RX=%d)\n", CAN_TX_PIN, CAN_RX_PIN);

    while (n2k_running) {
        NMEA2000.ParseMessages();
        vTaskDelay(pdMS_TO_TICKS(5));  // ~200 Hz polling
    }
    Serial.println("[N2K] Task ended");
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────────
void enable_nmea2000() {
    if (n2k_running) return;

    init_sensor_mutex();

    // Configure the NMEA2000 library
    NMEA2000.SetN2kCANMsgBufSize(8);      // 8-message TX buffer
    NMEA2000.SetN2kCANReceiveFrameBufSize(100); // 100 CAN frames RX buffer

    // Set product information (shows up when other devices query us)
    NMEA2000.SetProductInformation(
        "00000001",              // Serial number
        100,                     // Product code
        "Marine Display",        // Model ID
        "1.0.0",                 // Software version
        "1.0.0"                  // Model version
    );
    NMEA2000.SetDeviceInformation(
        1,                       // Unique number
        130,                     // Device function: Display
        120,                     // Device class: Display
        2046                     // Manufacturer code (not assigned — use 2046 = "Other")
    );

    // Listen-only mode: receive all PGNs, don't transmit heartbeats etc.
    NMEA2000.SetMode(tNMEA2000::N2km_ListenOnly, 25);
    NMEA2000.SetMsgHandler(handle_n2k_msg);

    // CAN bus is opened inside n2k_task on core 0 to keep the TWAI
    // interrupt off core 1 where the LCD GDMA ISR lives.
    n2k_running = true;
    xTaskCreatePinnedToCore(n2k_task, "N2K", 8192, nullptr, 3, &n2k_task_handle, 0);
    Serial.println("[N2K] NMEA 2000 task launched");
}

void disable_nmea2000() {
    if (!n2k_running) return;
    n2k_running = false;
    if (n2k_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));  // let task exit cleanly
        n2k_task_handle = nullptr;
    }
    Serial.println("[N2K] NMEA 2000 disabled");
}

bool is_nmea2000_running() {
    return n2k_running;
}

void pause_n2k_updates() {
    if (!n2k_updates_paused) {
        n2k_updates_paused = true;
        Serial.println("[N2K] Display updates paused (config page open)");
    }
}

void resume_n2k_updates() {
    if (n2k_updates_paused) {
        n2k_updates_paused = false;
        Serial.println("[N2K] Display updates resumed");
    }
}

bool is_n2k_updates_paused() {
    return n2k_updates_paused;
}
