#ifndef AUDIO_ALERT_H
#define AUDIO_ALERT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise ES8311 codec + I2S for tone playback.
// Call once from setup() after I2C bus (Wire) is available.
void audio_alert_init(void);

// Play a short beep pattern (200ms on, 100ms off, 200ms on).
// Non-blocking: spawns a one-shot FreeRTOS task.
void audio_alert_beep(void);

// Set alert volume (0–100 %). Writes ES8311 DAC volume register.
void audio_alert_set_volume(uint8_t pct);

// Flash zone IDs for per-zone flash (Dual/Quad in Per-screen mode)
#define FLASH_ZONE_FULL      -1   // whole-screen hardware inversion
#define FLASH_ZONE_DUAL_TOP   0
#define FLASH_ZONE_DUAL_BOT   1
#define FLASH_ZONE_QUAD_TL    2
#define FLASH_ZONE_QUAD_TR    3
#define FLASH_ZONE_QUAD_BL    4
#define FLASH_ZONE_QUAD_BR    5
#define FLASH_ZONE_COUNT      6

// Check alert and trigger buzzer if appropriate.
// Call from screen_render with the current value, alert thresholds, and screen index.
// flash_zone: FLASH_ZONE_FULL for whole-screen, or a specific zone for per-zone flash.
void check_alert(int32_t value, int16_t alert_low, int16_t alert_high,
                 uint8_t alert_flash, uint8_t alert_buzzer, int screen_idx,
                 int flash_zone);

// Returns true when the given zone should currently show inverted (flash phase on).
bool is_zone_flash_on(int zone_id);

// Returns true when a non-active screen has an alert (Global mode) and the
// 1-second flash phase is currently "on" — used to flash the alert icon.
bool is_remote_alert_on(void);

// Buzzer mode: 0=Off, 1=Global (all screens), 2=Per-screen (active only)
extern uint8_t buzzer_mode;
// Global alert volume 0-100 (persisted in NVS).
extern uint8_t alert_volume;
// Buzzer cooldown in seconds (persisted in NVS).
extern uint16_t buzzer_cooldown_sec;

// Start continuous display flash (1s normal, 1s inverted).
// Safe to call repeatedly while alert is active.
void display_alert_flash_start(void);

// Stop display flash and restore normal display.
void display_alert_flash_stop(void);

// Call at start/end of each update_screen_values cycle for flash tracking.
void alert_cycle_begin(void);
void alert_cycle_end(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_ALERT_H
