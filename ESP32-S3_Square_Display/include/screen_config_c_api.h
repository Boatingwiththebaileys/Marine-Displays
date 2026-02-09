// screen_config_c_api.h - C-friendly API for ScreenConfig
#ifndef SCREEN_CONFIG_C_API_H
#define SCREEN_CONFIG_C_API_H

#include "calibration_types.h" // For GaugeCalibrationPoint
#include <stdint.h>

// Avoid including C++-heavy headers here (UI C files include this header).
// If `NUM_SCREENS` is not defined by the build, default to 5.
#ifndef NUM_SCREENS
#define NUM_SCREENS 5
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Display type enumeration
typedef enum {
	DISPLAY_TYPE_GAUGE = 0,       // Traditional gauge with needles
	DISPLAY_TYPE_NUMBER = 1,      // Full screen number display
	DISPLAY_TYPE_DUAL = 2,        // Dual number display (top and bottom)
	DISPLAY_TYPE_QUAD = 3,        // Quad number display (4 quadrants)
	DISPLAY_TYPE_GAUGE_NUMBER = 4,// Gauge (top) + Number (center)
	DISPLAY_TYPE_GRAPH = 5        // LVGL chart/graph display
} DisplayType;

// Number display background type (uses bg_image field)
// bg_image = "" or "Default" -> transparent/default background
// bg_image = "/path/to/file.bin" -> bin file background
// bg_image = "Custom Color" -> use number_bg_color field

// Number display font size
typedef enum {
	NUMBER_FONT_SMALL = 0,     // 48pt (1x)
	NUMBER_FONT_MEDIUM = 1,    // 72pt (1.5x scale)
	NUMBER_FONT_LARGE = 2,     // 96pt (2x scale)
	NUMBER_FONT_XLARGE = 3,    // 120pt (2.5x scale)
	NUMBER_FONT_XXLARGE = 4    // 144pt (3x scale)
} NumberFontSize;

// Graph time range options
typedef enum {
	GRAPH_TIME_10S = 0,        // 10 seconds
	GRAPH_TIME_30S = 1,        // 30 seconds
	GRAPH_TIME_1M = 2,         // 1 minute
	GRAPH_TIME_5M = 3,         // 5 minutes
	GRAPH_TIME_10M = 4,        // 10 minutes
	GRAPH_TIME_30M = 5         // 30 minutes
} GraphTimeRange;

// Graph chart type
typedef enum {
	GRAPH_CHART_LINE = 0,      // Line chart
	GRAPH_CHART_BAR = 1,       // Bar/column chart
	GRAPH_CHART_SCATTER = 2    // Scatter plot
} GraphChartType;

typedef struct {
	GaugeCalibrationPoint cal[2][5]; // 2 gauges, 5 points each
	char icon_paths[2][128];         // 2 icons (top/bottom)
	uint8_t show_bottom;             // 1 = show bottom gauge/icon, 0 = hide (single gauge mode)
	char background_path[128];      // optional per-screen background image path
	uint8_t icon_pos[2];            // icon position per icon: 0=top,1=right,2=bottom,3=left
	float min[2][5];                 // min for each zone
	float max[2][5];                 // max for each zone
	char color[2][5][8];             // color hex for each zone
	int transparent[2][5];           // transparency for each zone
	int buzzer[2][5];                // buzzer enabled for each zone
	uint8_t display_type;            // DisplayType: 0=Gauge, 1=Number, 2=Dual
	// Number display settings (background uses background_path: empty/"Default"=default, bin path=file, "Custom Color"=use color)
	char number_bg_color[8];         // Hex color (e.g., "#FF0000") when background_path is "Custom Color"
	uint8_t number_font_size;        // NumberFontSize
	char number_font_color[8];       // Hex color for number text
	char number_path[128];           // Signal K path for number display
	// Dual display settings (top and bottom number displays)
	char dual_top_path[128];         // Signal K path for top display
	uint8_t dual_top_font_size;      // NumberFontSize for top display
	char dual_top_font_color[8];     // Hex color for top text
	char dual_bottom_path[128];      // Signal K path for bottom display
	uint8_t dual_bottom_font_size;   // NumberFontSize for bottom display
	char dual_bottom_font_color[8];  // Hex color for bottom text
	// Quad display settings (4 quadrant number displays)
	char quad_tl_path[128];          // Signal K path for top-left
	uint8_t quad_tl_font_size;       // NumberFontSize for top-left
	char quad_tl_font_color[8];      // Hex color for top-left
	char quad_tr_path[128];          // Signal K path for top-right
	uint8_t quad_tr_font_size;       // NumberFontSize for top-right
	char quad_tr_font_color[8];      // Hex color for top-right
	char quad_bl_path[128];          // Signal K path for bottom-left
	uint8_t quad_bl_font_size;       // NumberFontSize for bottom-left
	char quad_bl_font_color[8];      // Hex color for bottom-left
	char quad_br_path[128];          // Signal K path for bottom-right
	uint8_t quad_br_font_size;       // NumberFontSize for bottom-right
	char quad_br_font_color[8];      // Hex color for bottom-right
	// Gauge+Number display settings (top gauge + center number)
	char gauge_num_center_path[128]; // Signal K path for center number
	uint8_t gauge_num_center_font_size; // NumberFontSize for center number
	char gauge_num_center_font_color[8]; // Hex color for center number
	uint8_t graph_chart_type;         // GraphChartType for graph display
	uint8_t graph_time_range;         // GraphTimeRange for graph time window
	char graph_path_2[128];           // Signal K path for second graph series
	char graph_color_2[8];            // Hex color for second graph series
	// Add more fields as needed
} __attribute__((packed)) ScreenConfig;

// Expose the screen_configs array to C files
extern ScreenConfig screen_configs[NUM_SCREENS];

#ifdef __cplusplus
}
#endif

#endif // SCREEN_CONFIG_C_API_H
