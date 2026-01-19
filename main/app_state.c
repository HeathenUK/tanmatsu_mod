/**
 * @file app_state.c
 * @brief Application state management implementation
 */

#include "app_state.h"
#include <string.h>

// Global application state (singleton)
static app_state_t g_app_state = {0};
static bool g_app_state_initialized = false;

// Channel colors (RGB565) - same pattern repeated for all channels
static const uint16_t g_channel_colors[MOD_MAX_CHANNELS] = {
    0xF800, 0x07E0, 0x001F, 0xFFE0,  // Red, Green, Blue, Yellow
    0xF81F, 0x07FF, 0xFC00, 0x87E0,  // Magenta, Cyan, Orange, Lime
    0x041F, 0xF810, 0x87FF, 0xFC10,  // Light Blue, Pink, Light Cyan, Light Red
    0x87F0, 0x841F, 0xFFF0, 0x87FF,  // Light Green, Light Blue, Light Yellow, etc.
    0xFC1F, 0x87FF, 0xFFF0, 0x8410,
    0xFC10, 0x87F0, 0x841F, 0xFC10,
    0x87F0, 0x841F, 0xFFF0, 0x87FF,
    0xFC1F, 0x87FF, 0xFFF0, 0x8410,
    0xFC10, 0x87F0, 0x841F, 0xFC10,
    0x87F0, 0x841F, 0xFFF0, 0x87FF,
    0xFC1F, 0x87FF, 0xFFF0, 0x8410,
    0xFC10, 0x87F0, 0x841F, 0xFC10,
    0x87F0, 0x841F, 0xFFF0, 0x87FF,
    0xFC1F, 0x87FF, 0xFFF0, 0x8410,
};

app_state_t *app_state_init(void) {
    if (g_app_state_initialized) {
        return &g_app_state;
    }

    // Zero out the structure
    memset(&g_app_state, 0, sizeof(app_state_t));

    // Initialize default values
    g_app_state.current_view = APP_VIEW_TRACKER;
    g_app_state.view_key_pressed = false;
    g_app_state.channel_page_offset = 0;
    g_app_state.tab_pressed_this_frame = false;

    // VU meter state
    g_app_state.vu_state.cached_line_height = 32;
    g_app_state.vu_state.start_channel = 0;
    g_app_state.vu_state.visible_channels = 4;

    // File browser animation
    g_app_state.browser_anim.last_selected = -1;
    g_app_state.browser_anim.last_count = -1;
    g_app_state.browser_anim.last_start_idx = -1;

    // Tracker state
    g_app_state.tracker.last_row = -1;
    g_app_state.tracker.mod_info_loaded = false;
    g_app_state.tracker.current_tick_valid = false;

    // Internal flash
    g_app_state.int_flash_wl_handle = WL_INVALID_HANDLE;

    g_app_state_initialized = true;
    return &g_app_state;
}

app_state_t *app_state_get(void) {
    if (!g_app_state_initialized) {
        return app_state_init();
    }
    return &g_app_state;
}

void app_state_cleanup(void) {
    if (g_app_state.mod_file.data) {
        free(g_app_state.mod_file.data);
        g_app_state.mod_file.data = NULL;
        g_app_state.mod_file.size = 0;
    }

    if (g_app_state.spectrum) {
        spectrum_free(g_app_state.spectrum);
        g_app_state.spectrum = NULL;
    }

    g_app_state_initialized = false;
}

void app_state_reset_tracker(void) {
    g_app_state.tracker.tick_history_count = 0;
    g_app_state.tracker.current_tick_valid = false;
    g_app_state.tracker.last_row = -1;
    g_app_state.tracker.smooth_scroll_offset = 0;
    g_app_state.tracker.pending_new_row = false;
    g_app_state.tracker.mod_info_loaded = false;
    memset(g_app_state.tracker.tick_history, 0, sizeof(g_app_state.tracker.tick_history));
    memset(&g_app_state.tracker.current_tick, 0, sizeof(g_app_state.tracker.current_tick));
    memset(&g_app_state.tracker.pending_tick, 0, sizeof(g_app_state.tracker.pending_tick));
}

void app_state_reset_vu_meters(void) {
    for (int i = 0; i < MOD_MAX_CHANNELS; i++) {
        g_app_state.vu_state.levels[i] = 0.0f;
        g_app_state.vu_state.peaks[i] = 0.0f;
        g_app_state.vu_state.peak_hold[i] = 0;
    }
}

const uint16_t *app_state_get_channel_colors(void) {
    return g_channel_colors;
}
