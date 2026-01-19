/**
 * @file app_state.h
 * @brief Application state management for Tanmatsu MOD player
 *
 * This module manages all global application state including:
 * - Framebuffers and display state
 * - Playback view modes (tracker, spectrum, info)
 * - Channel pagination and VU meter state
 * - File browser state
 * - Module playback state
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mod_backend.h"
#include "xmp_compat.h"
#include "file_browser.h"
#include "spectrum_analyzer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "driver/ppa.h"

#ifdef __cplusplus
extern "C" {
#endif

// Framebuffer dimensions (defined in hw_accel.h but also needed here)
#define APP_STATE_FB_WIDTH  800
#define APP_STATE_FB_HEIGHT 480

// Screen margins
#define APP_STATE_MARGIN_LEFT   10
#define APP_STATE_MARGIN_RIGHT  10
#define APP_STATE_MARGIN_TOP    5
#define APP_STATE_MARGIN_BOTTOM 5

// Content area (after margins)
#define APP_STATE_CONTENT_WIDTH  (APP_STATE_FB_WIDTH - APP_STATE_MARGIN_LEFT - APP_STATE_MARGIN_RIGHT)
#define APP_STATE_CONTENT_HEIGHT (APP_STATE_FB_HEIGHT - APP_STATE_MARGIN_TOP - APP_STATE_MARGIN_BOTTOM)

// VU meter constants
#define APP_STATE_VU_DECAY_RATE       0.08f
#define APP_STATE_VU_ATTACK_RATE      0.4f
#define APP_STATE_VU_PEAK_DECAY_RATE  0.02f
#define APP_STATE_VU_PEAK_HOLD_FRAMES 20

// Tracker constants
#define APP_STATE_MAX_TRACKER_ROWS 60
#define APP_STATE_MAX_MOD_FILE_SIZE (10 * 1024 * 1024)  // 10MB

// File browser animation
#define APP_STATE_BROWSER_ANIM_DURATION_MS 100

/**
 * @brief Playback view modes
 */
typedef enum {
    APP_VIEW_TRACKER,    // Normal tracker display with channel data
    APP_VIEW_SPECTRUM,   // FFT spectrum analyzer visualization
    APP_VIEW_INFO,       // Module info display
    APP_VIEW_VGM         // VGM playback view
} app_playback_view_t;

/**
 * @brief Tracker tick data structure (one row of pattern data)
 */
typedef struct {
    struct xmp_channel_info channels[MOD_MAX_CHANNELS];
    int pos;
    int pattern;
    int row;
    int num_channels;
} app_tracker_tick_t;

/**
 * @brief VU meter state structure
 */
typedef struct {
    float levels[MOD_MAX_CHANNELS];
    float peaks[MOD_MAX_CHANNELS];
    uint32_t peak_hold[MOD_MAX_CHANNELS];
    // Layout cache (updated by tracker rendering, used by VU overlay)
    int cached_line_height;
    int start_channel;
    int visible_channels;
    int channel_x[4];
    int channel_width[4];
} app_vu_state_t;

/**
 * @brief File browser animation state
 */
typedef struct {
    int target_y;
    int current_y;
    int start_y;
    TickType_t start_tick;
    int last_selected;
    int last_count;
    int last_start_idx;
} app_browser_anim_state_t;

/**
 * @brief Tracker display state
 */
typedef struct {
    app_tracker_tick_t tick_history[APP_STATE_MAX_TRACKER_ROWS];
    int tick_history_count;
    app_tracker_tick_t current_tick;
    bool current_tick_valid;
    int last_row;
    struct xmp_module_info mod_info;
    bool mod_info_loaded;
    int smooth_scroll_offset;
    bool pending_new_row;
    app_tracker_tick_t pending_tick;
} app_tracker_state_t;

/**
 * @brief MOD file state
 */
typedef struct {
    uint8_t *data;
    size_t size;
    char path[MAX_FILENAME_LEN];
} app_mod_file_state_t;

/**
 * @brief Complete application state
 */
typedef struct {
    // Framebuffers
    uint16_t *fb;
    uint16_t *fb_rotated;

    // PPA handles for hardware acceleration
    ppa_client_handle_t ppa_srm_handle;
    ppa_client_handle_t ppa_fill_handle;

    // Input queue
    QueueHandle_t input_event_queue;

    // View state
    app_playback_view_t current_view;
    bool view_key_pressed;
    int channel_page_offset;
    bool tab_pressed_this_frame;

    // VU meter state
    app_vu_state_t vu_state;

    // Spectrum analyzer
    spectrum_analyzer_t *spectrum;

    // File browser
    file_browser_t browser;
    bool browser_active;
    app_browser_anim_state_t browser_anim;

    // Tracker state
    app_tracker_state_t tracker;

    // MOD file state
    app_mod_file_state_t mod_file;

    // Internal flash wear levelling
    wl_handle_t int_flash_wl_handle;
} app_state_t;

/**
 * @brief Initialize application state
 *
 * @return Pointer to application state structure
 */
app_state_t *app_state_init(void);

/**
 * @brief Get global application state
 *
 * @return Pointer to application state structure
 */
app_state_t *app_state_get(void);

/**
 * @brief Cleanup application state
 */
void app_state_cleanup(void);

/**
 * @brief Reset tracker state (e.g., when loading new module)
 */
void app_state_reset_tracker(void);

/**
 * @brief Reset VU meter levels
 */
void app_state_reset_vu_meters(void);

/**
 * @brief Get channel colors for tracker display
 *
 * @return Pointer to array of RGB565 colors
 */
const uint16_t *app_state_get_channel_colors(void);

#ifdef __cplusplus
}
#endif
