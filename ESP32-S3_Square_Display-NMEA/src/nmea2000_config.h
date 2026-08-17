#ifndef NMEA2000_CONFIG_H
#define NMEA2000_CONFIG_H

#include <Arduino.h>

// CAN GPIO pins for Waveshare ESP32-S3-Touch-LCD-4 (TJA1051 transceiver)
#ifndef CAN_TX_PIN
#define CAN_TX_PIN 6
#endif
#ifndef CAN_RX_PIN
#define CAN_RX_PIN 0
#endif

// Initialise the NMEA 2000 bus and start the receive task (core 0).
// Call after WiFi/network setup (WiFi stays on for config pages).
void enable_nmea2000();

// Stop the receive task and shut down the CAN peripheral.
void disable_nmea2000();

// Returns true while the N2K task is running.
bool is_nmea2000_running();

// Total PGN messages received since enable_nmea2000().
uint32_t get_n2k_msg_count();

// Pause / resume display updates (N2K CAN task keeps running).
// Used to prevent LVGL mutations while the config page is open.
void pause_n2k_updates();
void resume_n2k_updates();
bool is_n2k_updates_paused();

// ── PGN field identifiers used in the config UI dropdown ─────────────
// Each value maps to a specific field inside a decoded PGN message.
// The numeric value encodes  (PGN << 8) | field_index  but the enum
// names are human-readable for the web UI.
enum N2kField : uint16_t {
    N2K_NONE                   = 0,

    // ── Engine (PGN 127488 — Rapid, PGN 127489 — Dynamic) ───────────
    N2K_ENGINE_RPM             = 1,
    N2K_ENGINE_BOOST           = 2,
    N2K_ENGINE_TILT_TRIM       = 3,
    N2K_OIL_PRESSURE           = 10,
    N2K_OIL_TEMPERATURE        = 11,
    N2K_COOLANT_TEMPERATURE    = 12,
    N2K_ALT_VOLTAGE            = 13,
    N2K_FUEL_RATE              = 14,
    N2K_ENGINE_HOURS           = 15,
    N2K_ENGINE_LOAD            = 16,
    N2K_ENGINE_TORQUE          = 17,
    N2K_EXHAUST_TEMPERATURE    = 18,
    // Engine #2 (twin engines — same PGNs, instance 1)
    N2K_ENGINE2_RPM            = 100,
    N2K_ENGINE2_OIL_PRESSURE   = 101,
    N2K_ENGINE2_OIL_TEMP       = 102,
    N2K_ENGINE2_COOLANT_TEMP   = 103,
    N2K_ENGINE2_ALT_VOLTAGE    = 104,
    N2K_ENGINE2_FUEL_RATE      = 105,
    N2K_ENGINE2_HOURS          = 106,
    N2K_ENGINE2_EXHAUST_TEMP   = 107,

    // ── Transmission (PGN 127493) ────────────────────────────────────
    N2K_TRANS_GEAR             = 110,
    N2K_TRANS_OIL_PRESSURE     = 111,
    N2K_TRANS_OIL_TEMP         = 112,

    // ── Engine Trip (PGN 127497) ─────────────────────────────────────
    N2K_TRIP_FUEL_USED         = 115,
    N2K_FUEL_RATE_AVG          = 116,
    N2K_FUEL_RATE_ECONOMY      = 117,

    // ── Fluid Levels (PGN 127505) ────────────────────────────────────
    N2K_FUEL_LEVEL             = 20,
    N2K_FRESHWATER_LEVEL       = 21,
    N2K_WASTEWATER_LEVEL       = 22,
    N2K_OIL_LEVEL              = 23,
    N2K_BLACKWATER_LEVEL       = 24,
    N2K_FUEL_CAPACITY          = 25,
    N2K_FRESHWATER_CAPACITY    = 26,
    N2K_LIVEWELL_LEVEL         = 27,

    // ── Navigation ───────────────────────────────────────────────────
    // PGN 127250 — Vessel Heading
    N2K_HEADING                = 30,
    N2K_MAGNETIC_DEVIATION     = 31,
    N2K_MAGNETIC_VARIATION_HDG = 32,
    // PGN 127251 — Rate of Turn
    N2K_RATE_OF_TURN           = 33,
    // PGN 127258 — Magnetic Variation
    N2K_MAGNETIC_VARIATION     = 34,
    // PGN 129025 — Position, Rapid Update
    N2K_LATITUDE               = 40,
    N2K_LONGITUDE              = 41,
    // PGN 129026 — COG & SOG, Rapid Update
    N2K_COG                    = 42,
    N2K_SOG                    = 43,
    // PGN 129029 — GNSS Position Data
    N2K_GNSS_DATETIME          = 80,
    N2K_GNSS_ALTITUDE          = 81,
    N2K_GNSS_SATELLITES        = 82,
    N2K_GNSS_HDOP              = 83,
    // PGN 129283 — Cross Track Error
    N2K_XTE                    = 44,
    // PGN 129284 — Navigation Data (waypoint)
    N2K_WPT_DISTANCE           = 45,
    N2K_WPT_BEARING            = 46,
    N2K_WPT_VMG                = 47,
    N2K_ETA_TIME               = 48,
    // PGN 128259 — Speed, Water Referenced
    N2K_SPEED_WATER            = 49,
    N2K_SPEED_GROUND           = 120,
    // PGN 128275 — Distance Log
    N2K_LOG_TOTAL              = 121,
    N2K_LOG_TRIP               = 122,
    // PGN 128000 — Leeway
    N2K_LEEWAY                 = 123,

    // ── Depth (PGN 128267) ───────────────────────────────────────────
    N2K_WATER_DEPTH            = 50,
    N2K_DEPTH_OFFSET           = 51,
    N2K_DEPTH_BELOW_KEEL       = 52,  // computed: depth + offset

    // ── Wind (PGN 130306) ────────────────────────────────────────────
    N2K_WIND_SPEED_APPARENT    = 60,
    N2K_WIND_ANGLE_APPARENT    = 61,
    N2K_WIND_SPEED_TRUE        = 62,
    N2K_WIND_ANGLE_TRUE        = 63,

    // ── Environment ──────────────────────────────────────────────────
    // PGN 130312 — Temperature
    N2K_TEMPERATURE            = 70,
    N2K_SEA_TEMPERATURE        = 71,
    N2K_TEMPERATURE_SET        = 72,
    // PGN 130311 — Environmental Parameters
    N2K_OUTSIDE_TEMP           = 73,
    N2K_OUTSIDE_HUMIDITY       = 74,
    N2K_ATMOSPHERIC_PRESSURE   = 75,
    // PGN 130314 — Actual Pressure
    N2K_BAROMETRIC_PRESSURE    = 76,
    // PGN 130316 — Temperature Extended Range
    N2K_FRIDGE_TEMPERATURE     = 77,
    N2K_FREEZER_TEMPERATURE    = 78,
    N2K_INSIDE_TEMPERATURE     = 79,   // source 2: InsideTemperature
    N2K_CABIN_TEMPERATURE      = 85,   // source 4: MainCabinTemperature
    N2K_HEATING_TEMPERATURE    = 86,   // source 8: HeatingSystemTemperature
    N2K_DEWPOINT_TEMPERATURE   = 87,   // source 9: DewPointTemperature
    N2K_WINDCHILL_APPARENT     = 88,   // source 10: ApparentWindChillTemperature
    N2K_WINDCHILL_THEORETICAL  = 89,   // source 11: TheoreticalWindChillTemperature

    // ── Attitude (PGN 127257) ────────────────────────────────────────
    N2K_PITCH                  = 130,
    N2K_ROLL                   = 131,
    N2K_YAW                    = 132,

    // ── Rudder (PGN 127245) ──────────────────────────────────────────
    N2K_RUDDER_POSITION        = 135,

    // ── Trim Tabs (PGN 130576) ───────────────────────────────────────
    N2K_TRIM_TAB_PORT          = 136,
    N2K_TRIM_TAB_STBD          = 137,

    // ── Battery / Electrical ─────────────────────────────────────────
    // PGN 127508 — Battery Status
    N2K_BATTERY_VOLTAGE        = 90,
    N2K_BATTERY_CURRENT        = 91,
    N2K_BATTERY_TEMPERATURE    = 92,
    N2K_BATTERY2_VOLTAGE       = 93,
    N2K_BATTERY2_CURRENT       = 94,
    // PGN 127506 — DC Detailed Status
    N2K_BATTERY_SOC            = 95,
    N2K_BATTERY_TIME_REMAIN    = 96,
    // PGN 127751 — DC Voltage/Current
    N2K_DC_SOURCE_VOLTAGE      = 97,
    N2K_DC_SOURCE_CURRENT      = 98,

    // PGN 126992 — System Time
    N2K_SYSTEM_TIME            = 140,

    N2K_FIELD_COUNT            // sentinel
};

// Human-readable label for a given N2kField (used in web UI dropdown).
const char* n2k_field_label(N2kField f);

// SI unit string for a given N2kField (used by convert_unit for display).
const char* n2k_field_unit(N2kField f);

// Map a SignalK-style path string (stored in screen_configs) to an N2kField.
// e.g. "propulsion.*.revolutions" -> N2K_ENGINE_RPM
// Returns N2K_NONE if no mapping exists.
N2kField n2k_field_from_path(const String& path);

// Map a stored config value to an N2kField.  Tries numeric enum first
// (e.g. "1" → N2K_ENGINE_RPM), then falls back to n2k_field_from_path()
// for legacy SignalK path strings.
N2kField n2k_field_from_stored(const String& stored);

#endif // NMEA2000_CONFIG_H
