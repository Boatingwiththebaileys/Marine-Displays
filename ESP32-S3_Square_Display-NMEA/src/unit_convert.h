#ifndef UNIT_CONVERT_H
#define UNIT_CONVERT_H

#include <Arduino.h>

// Unit system presets matching SignalK conventions
enum UnitSystem : uint8_t {
    UNIT_METRIC           = 0,  // km/h, km, m, °C, L, bar
    UNIT_IMPERIAL_US      = 1,  // mph, mi, ft, °F, US gal, PSI
    UNIT_IMPERIAL_UK      = 2,  // mph, mi, ft, °C, UK gal, PSI
    UNIT_NAUTICAL_METRIC  = 3,  // kn, nm, m, °C, L, bar        (current default)
    UNIT_NAUTICAL_IMP_US  = 4,  // kn, nm, ft, °F, US gal, PSI
    UNIT_NAUTICAL_IMP_UK  = 5,  // kn, nm, ft, °C, UK gal, PSI
    UNIT_SYSTEM_COUNT     = 6
};

extern UnitSystem unit_system;

// Human-readable name for the dropdown / web UI
const char* unit_system_name(UnitSystem sys);

// Infer the SI unit from a SignalK path when metadata hasn't arrived yet.
// Returns e.g. "K" for temperature paths, "Pa" for pressure, etc.
String infer_unit_from_path(const String& path);

// Convert a SignalK SI value + unit string to the display value/unit
// for the currently selected unit system.
// Returns the converted value; writes the display-unit string into `out_unit`.
float convert_unit(float si_value, const String& si_unit, String& out_unit);

// Overload: if si_unit is empty, infer it from the SignalK path.
float convert_unit(float si_value, const String& si_unit, const String& path, String& out_unit);

// Convenience: convert speed (m/s) → display speed for selected system
float convert_speed(float ms);

// Get the number of decimal places for displaying a value, based on its
// description string (from n2k_field_label()).  Returns 0, 1, or 2.
uint8_t get_display_decimals(const char* description);

// Format a display value into buf using the correct decimal places.
void format_display_value(char* buf, size_t len, float value, const char* description);
const char* speed_unit_label();

// Convenience: convert angle (rad) → degrees (same for all systems)
float convert_angle_rad(float rad);

#endif
