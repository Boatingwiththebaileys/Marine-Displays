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

    // Distance: m → km, mi, nm, ft
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
