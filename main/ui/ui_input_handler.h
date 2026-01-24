/**
 * @file ui_input_handler.h
 * @brief Input event handling for Tanmatsu MOD player
 *
 * This module processes input events and returns actions for the main loop to execute.
 * It handles navigation, scancodes, and provides a clean separation between input
 * processing and application state management.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "bsp/input.h"
#include "file_browser.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Input handler result types
 */
typedef enum {
    INPUT_ACTION_NONE = 0,           // No action needed
    INPUT_ACTION_REDRAW_BROWSER,     // Redraw file browser
    INPUT_ACTION_LOAD_MOD_FILE,      // Load MOD file from path
    INPUT_ACTION_EXIT_TO_LAUNCHER,   // Exit to launcher
    INPUT_ACTION_RETURN_TO_BROWSER,  // Return from playback to browser
    INPUT_ACTION_SET_VOLUME,         // Set audio volume
    INPUT_ACTION_SET_VIEW_MODE,      // Set view mode (tracker/spectrum/info)
    INPUT_ACTION_SET_CHANNEL_PAGE,   // Set channel pagination
    INPUT_ACTION_PAUSE_RESUME,       // Pause/resume playback
    INPUT_ACTION_TOGGLE_CHANNEL_MUTE, // Toggle channel mute
    INPUT_ACTION_VGM_SKIP,            // Skip ahead in VGM playback
    INPUT_ACTION_TOGGLE_BACKLIGHT     // Toggle display/keyboard backlight
} input_action_type_t;

/**
 * @brief Input handler result data
 */
typedef struct {
    input_action_type_t action;
    union {
        struct {
            char path[256];  // Path to MOD file to load
        } load_mod;
        struct {
            float volume;    // New volume level (0.2-1.0)
        } set_volume;
        struct {
            int view_mode;   // View mode index
        } set_view;
        struct {
            bool page_next;  // True = next page, false = previous page
        } set_channel_page;
        struct {
            int channel;     // Channel number to toggle mute
        } toggle_mute;
        struct {
            uint32_t seconds; // Seconds to skip ahead
        } vgm_skip;
    } data;
} input_action_result_t;

/**
 * @brief Input handler context (state needed for input processing)
 */
typedef struct {
    // Browser state
    file_browser_t *browser;
    bool *browser_active;

    // Playback state
    bool playing;
    bool paused;
    float current_volume;
    int current_view;

    // Flags (output - set by handler)
    bool *tab_pressed;
    bool *view_key_pressed;
} input_handler_context_t;

/**
 * @brief Process a single input event
 *
 * @param event Input event from BSP
 * @param ctx Input handler context
 * @param result Output action result
 * @return ESP_OK on success
 */
esp_err_t ui_input_handle_event(const bsp_input_event_t *event,
                                 input_handler_context_t *ctx,
                                 input_action_result_t *result);

/**
 * @brief Handle navigation input (UP/DOWN/LEFT/RIGHT)
 *
 * @param key Navigation key
 * @param ctx Input handler context
 * @param result Output action result
 * @return ESP_OK on success
 */
esp_err_t ui_input_handle_navigation(bsp_input_navigation_key_t key,
                                      input_handler_context_t *ctx,
                                      input_action_result_t *result);

/**
 * @brief Handle scancode input (F-keys, space, number keys, etc.)
 *
 * @param scancode Scancode from keyboard
 * @param ctx Input handler context
 * @param result Output action result
 * @return ESP_OK on success
 */
esp_err_t ui_input_handle_scancode(bsp_input_scancode_t scancode,
                                    input_handler_context_t *ctx,
                                    input_action_result_t *result);

#ifdef __cplusplus
}
#endif
