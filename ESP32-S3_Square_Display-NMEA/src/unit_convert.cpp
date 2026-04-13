#include "unit_convert.h"
#include <math.h>

UnitSystem unit_system = UNIT_NAUTICAL_METRIC;  // default: knots / °C / mbar

static const char* const system_names[] = {
    "Metric",
    "Imperial (US)",
    "Imperial (UK)",
    "Nautical Metric",
    "Nautical Imperial (US)",
    "Nautical Imperial (UK)"
};

const char* unit_system_name(UnitSystem sys) {
    if (sys >= UNIT_SYSTEM_COUNT) return "Unknown";
    return system_names[sys];
}

// --- helper predicates based on system flags ---

static bool uses_fahrenheit() {
    return unit_system == UNIT_IMPERIAL_US || unit_system == UNIT_NAUTICAL_IMP_US;
}

static bool uses_psi() {
    return unit_system == UNIT_IMPERIAL_US  || unit_system == UNIT_IMPERIAL_UK ||
           unit_system == UNIT_NAUTICAL_IMP_US || unit_system == UNIT_NAUTICAL_IMP_UK;
}

static bool uses_knots() {
    return unit_system == UNIT_NAUTICAL_METRIC ||
           unit_system == UNIT_NAUTICAL_IMP_US ||
           unit_system == UNIT_NAUTICAL_IMP_UK;
}

static bool uses_mph() {
    return unit_system == UNIT_IMPERIAL_US || unit_system == UNIT_IMPERIAL_UK;
}

static bool uses_feet() {
    return unit_system == UNIT_IMPERIAL_US  || unit_system == UNIT_IMPERIAL_UK ||
           unit_system == UNIT_NAUTICAL_IMP_US || unit_system == UNIT_NAUTICAL_IMP_UK;
}

// --- public API ---

// Infer SI unit from SignalK path when metadata hasn't arrived yet
String infer_unit_from_path(const String& path) {
    if (path.indexOf("temperature") >= 0 || path.indexOf("Temperature") >= 0) return "K";
    if (path.indexOf("pressure") >= 0 || path.indexOf("Pressure") >= 0)       return "Pa";
    if (path.indexOf("revolutions") >= 0)                                       return "Hz";
    if (path.indexOf("currentLevel") >= 0 || path.indexOf("capacity") >= 0)    return "ratio";
    if (path.indexOf("speed") >= 0 || path.indexOf("Speed") >= 0)             return "m/s";
    if (path.indexOf("heading") >= 0 || path.indexOf("bearing") >= 0 ||
        path.indexOf("course") >= 0 || path.indexOf("angle") >= 0 ||
        path.indexOf("Heading") >= 0 || path.indexOf("Course") >= 0)          return "rad";
    if (path.indexOf("volume") >= 0)                                            return "m3";
    if (path.indexOf("depth") >= 0 || path.indexOf("draft") >= 0 ||
        path.indexOf("length") >= 0 || path.indexOf("beam") >= 0 ||
        path.indexOf("height") >= 0)                                              return "m";
    return "";
}

float convert_unit(float si_value, const String& si_unit, String& out_unit) {
    if (isnan(si_value)) { out_unit = si_unit; return si_value; }

    // Temperature: K → °C or °F
    if (si_unit == "K") {
        float c = si_value - 273.15f;
        if (uses_fahrenheit()) {
            out_unit = String("\xC2\xB0") + "F";
            return c * 1.8f + 32.0f;
        }
        out_unit = String("\xC2\xB0") + "C";
        return c;
    }

    // Pressure: Pa → mbar or PSI
    if (si_unit == "Pa") {
        if (uses_psi()) {
            out_unit = "PSI";
            return si_value * 0.000145038f;
        }
        out_unit = "mbar";
        return si_value / 100.0f;
    }

    // Ratio → %
    if (si_unit == "ratio") {
        out_unit = "%";
        return si_value * 100.0f;
    }

    // Frequency: Hz → RPM
    if (si_unit == "Hz") {
        out_unit = "RPM";
        return si_value * 60.0f;
    }

    // Speed: m/s → kn, km/h, or mph
    if (si_unit == "m/s") {
        out_unit = speed_unit_label();
        return convert_speed(si_value);
    }

    // Angle: rad → degrees (universal)
    if (si_unit == "rad") {
        out_unit = String("\xC2\xB0");
        return si_value * 57.2957795f;
    }

    // Volume: m³ → L, US gal, UK gal
    if (si_unit == "m3") {
        if (unit_system == UNIT_IMPERIAL_US || unit_system == UNIT_NAUTICAL_IMP_US) {
            out_unit = "gal";
            return si_value * 264.172f;
        }
        if (unit_system == UNIT_IMPERIAL_UK || unit_system == UNIT_NAUTICAL_IMP_UK) {
            out_unit = "gal";
            return si_value * 219.969f;
        }
        out_unit = "L";
        return si_value * 1000.0f;
    }

    // Depth: m_depth → m or ft  (short-distance measurement)
    if (si_unit == "m_depth") {
        if (uses_feet()) {
            out_unit = "ft";
            return si_value * 3.28084f;
        }
        out_unit = "m";
        return si_value;
    }

    // Distance: m → km, mi, nm, ft  (if SignalK ever sends "m")
    if (si_unit == "m") {
        if (uses_knots()) {
            out_unit = "nm";
            return si_value / 1852.0f;
        }
        if (uses_mph()) {
            out_unit = "mi";
            return si_value / 1609.344f;
        }
        out_unit = "km";
        return si_value / 1000.0f;
    }

    // No conversion needed — pass through
    out_unit = si_unit;
    return si_value;
}

float convert_speed(float ms) {
    if (isnan(ms)) return ms;
    if (uses_knots())  return ms * 1.94384f;
    if (uses_mph())    return ms * 2.23694f;
    return ms * 3.6f;  // km/h
}

const char* speed_unit_label() {
    if (uses_knots()) return "kn";
    if (uses_mph())   return "mph";
    return "km/h";
}

float convert_angle_rad(float rad) {
    if (isnan(rad)) return rad;
    return rad * 57.2957795f;
}

// ── Decimal-place table keyed by n2k_field_label() description strings ──
struct DecEntry { const char* label; uint8_t dp; };
static const DecEntry decimal_table[] = {
    // Engine
    {"Engine RPM",              0},
    {"Engine Boost",            0},
    {"Tilt / Trim",             1},
    {"Oil Pressure",            0},
    {"Oil Temperature",         0},
    {"Coolant Temperature",     0},
    {"Alternator Voltage",      2},
    {"Fuel Rate",               1},
    {"Engine Hours",            0},
    {"Engine Load",             0},
    {"Engine Torque",           0},
    {"Exhaust Temperature",     0},
    // Engine #2
    {"Engine 2 RPM",            0},
    {"Engine 2 Oil Pressure",   0},
    {"Engine 2 Oil Temp",       0},
    {"Engine 2 Coolant Temp",   0},
    {"Engine 2 Alt Voltage",    2},
    {"Engine 2 Fuel Rate",      1},
    {"Engine 2 Hours",          0},
    {"Engine 2 Exhaust Temp",   0},
    // Transmission
    {"Transmission Gear",       0},
    {"Trans Oil Pressure",      0},
    {"Trans Oil Temp",          0},
    // Trip / Fuel
    {"Trip Fuel Used",          1},
    {"Avg Fuel Rate",           1},
    {"Fuel Economy",            1},
    {"Fuel Level",              0},
    {"Fuel Capacity",           0},
    // Fluid levels
    {"Fresh Water Level",       0},
    {"Waste Water Level",       0},
    {"Oil Level",               0},
    {"Black Water Level",       0},
    {"Fresh Water Capacity",    0},
    {"Live Well Level",         0},
    // Navigation
    {"Heading",                 0},
    {"Mag Deviation",           1},
    {"Mag Variation (Hdg)",     1},
    {"Rate of Turn",            1},
    {"Magnetic Variation",      1},
    {"Latitude",                6},
    {"Longitude",               6},
    {"COG",                     0},
    {"SOG",                     1},
    {"GNSS Altitude",           1},
    {"GNSS Satellites",         0},
    {"GNSS HDOP",               1},
    {"Cross Track Error",       2},
    {"Waypoint Distance",       1},
    {"Waypoint Bearing",        0},
    {"VMG to Waypoint",         1},
    {"Speed (Water)",           1},
    {"Speed (Ground)",          1},
    {"Total Log",               1},
    {"Trip Log",                1},
    {"Leeway",                  1},
    // Depth
    {"Water Depth",             1},
    {"Depth Offset",            1},
    // Wind
    {"Wind Speed (Apparent)",   1},
    {"Wind Angle (Apparent)",   0},
    {"Wind Speed (True)",       1},
    {"Wind Angle (True)",       0},
    // Environment
    {"Temperature",             0},
    {"Sea Temperature",         1},
    {"Set Temperature",         0},
    {"Outside Air Temp",        0},
    {"Humidity",                0},
    {"Atmospheric Pressure",    0},
    {"Barometric Pressure",     0},
    // Attitude
    {"Pitch",                   1},
    {"Roll",                    1},
    {"Yaw",                     1},
    // Rudder / Trim
    {"Rudder Position",         1},
    {"Trim Tab (Port)",         1},
    {"Trim Tab (Starboard)",    1},
    // Battery / Electrical
    {"Battery Voltage",         2},
    {"Battery Current",         1},
    {"Battery Temperature",     0},
    {"Battery 2 Voltage",       2},
    {"Battery 2 Current",       1},
    {"Battery SOC",             0},
    {"Battery Time Remaining",  0},
    {"DC Source Voltage",       2},
    {"DC Source Current",       1},
    // Time
    {"System Time",             0},
    {nullptr, 1}  // sentinel — default 1 dp
};

uint8_t get_display_decimals(const char* description) {
    if (!description || description[0] == '\0') return 1;
    for (const DecEntry* e = decimal_table; e->label; ++e)
        if (strcmp(description, e->label) == 0) return e->dp;
    return 1;  // sensible default
}

void format_display_value(char* buf, size_t len, float value, const char* description) {
    uint8_t dp = get_display_decimals(description);
    switch (dp) {
        case 0:  snprintf(buf, len, "%.0f", value); break;
        case 2:  snprintf(buf, len, "%.2f", value); break;
        case 6:  snprintf(buf, len, "%.6f", value); break;
        default: snprintf(buf, len, "%.1f", value); break;
    }
}

float convert_unit(float si_value, const String& si_unit, const String& path, String& out_unit) {
    String effective_unit = si_unit;
    if (effective_unit.length() == 0) {
        effective_unit = infer_unit_from_path(path);
    }

    // Depth / vessel dimension paths: convert m → ft for imperial, otherwise keep m
    if (effective_unit == "m" &&
        (path.indexOf("depth") >= 0 || path.indexOf("draft") >= 0 ||
         path.indexOf("length") >= 0 || path.indexOf("beam") >= 0 ||
         path.indexOf("height") >= 0)) {
        if (uses_feet()) {
            out_unit = "ft";
            return si_value * 3.28084f;
        }
        out_unit = "m";
        return si_value;
    }

    return convert_unit(si_value, effective_unit, out_unit);
}
