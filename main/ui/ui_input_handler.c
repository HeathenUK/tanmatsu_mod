/**
 * @file ui_input_handler.c
 * @brief Input event handling implementation
 */

#include "ui_input_handler.h"
#include "file_browser.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>

static const char TAG[] __attribute__((unused)) = "ui_input";

esp_err_t ui_input_handle_event(const bsp_input_event_t *event,
                                 input_handler_context_t *ctx,
                                 input_action_result_t *result) {
    static int input_log_count = 0;
    if (!event || !ctx || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize result
    memset(result, 0, sizeof(input_action_result_t));
    result->action = INPUT_ACTION_NONE;

    switch (event->type) {
        case INPUT_EVENT_TYPE_NAVIGATION:
            if (input_log_count < 20) {
                ESP_LOGI(TAG, "NAV event: key=%d state=%d", event->args_navigation.key, event->args_navigation.state);
                input_log_count++;
            }
            if (event->args_navigation.state) {  // Only process key press (not release)
                return ui_input_handle_navigation(event->args_navigation.key, ctx, result);
            }
            break;

        case INPUT_EVENT_TYPE_KEYBOARD:
            // Keyboard events not currently used
            break;

        case INPUT_EVENT_TYPE_ACTION:
            // Power button handler removed - using F6 instead
            break;

        case INPUT_EVENT_TYPE_SCANCODE:
            if (input_log_count < 40) {
                ESP_LOGI(TAG, "SCANCODE event: 0x%02X", event->args_scancode.scancode);
                input_log_count++;
            } else if (event->args_scancode.scancode == 0x40) {
                ESP_LOGI(TAG, "SCANCODE event: 0x40 (F6)");
            }
            return ui_input_handle_scancode(event->args_scancode.scancode, ctx, result);

        default:
            break;
    }

    return ESP_OK;
}

esp_err_t ui_input_handle_navigation(bsp_input_navigation_key_t key,
                                      input_handler_context_t *ctx,
                                      input_action_result_t *result) {
    if (!ctx || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    // Browser navigation
    if (*ctx->browser_active) {
        switch (key) {
            case BSP_INPUT_NAVIGATION_KEY_UP:
                file_browser_up(ctx->browser);
                result->action = INPUT_ACTION_REDRAW_BROWSER;
                break;

            case BSP_INPUT_NAVIGATION_KEY_DOWN:
                file_browser_down(ctx->browser);
                result->action = INPUT_ACTION_REDRAW_BROWSER;
                break;

            case BSP_INPUT_NAVIGATION_KEY_LEFT:
                if (file_browser_back(ctx->browser) == ESP_OK) {
                    result->action = INPUT_ACTION_REDRAW_BROWSER;
                }
                break;

            case BSP_INPUT_NAVIGATION_KEY_RIGHT: {
                // Enter directory or select file
                bool is_directory = false;
                if (ctx->browser->selected_index < ctx->browser->count) {
                    is_directory = ctx->browser->files[ctx->browser->selected_index].is_dir;
                }

                char selected_path[256];
                esp_err_t select_res = file_browser_select(ctx->browser, selected_path, sizeof(selected_path));

                if (select_res == ESP_OK && !is_directory) {
                    // File selected - check if it's a supported music file
                    const char *ext = strrchr(selected_path, '.');
                    if (ext && (strcasecmp(ext, ".mod") == 0 || strcasecmp(ext, ".xm") == 0 ||
                                strcasecmp(ext, ".s3m") == 0 || strcasecmp(ext, ".it") == 0 ||
                                strcasecmp(ext, ".vgm") == 0 || strcasecmp(ext, ".vgz") == 0)) {
                        // Valid music file - request load
                        result->action = INPUT_ACTION_LOAD_MOD_FILE;
                        strncpy(result->data.load_mod.path, selected_path, sizeof(result->data.load_mod.path) - 1);
                        result->data.load_mod.path[sizeof(result->data.load_mod.path) - 1] = '\0';
                    }
                } else if (select_res == ESP_OK) {
                    // Directory entered
                    result->action = INPUT_ACTION_REDRAW_BROWSER;
                }
                break;
            }

            default:
                break;
        }
    }
    // Volume control / navigation during playback
    else if (ctx->playing) {
        float new_vol = ctx->current_volume;
        if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
            // Treat LEFT as "back" during playback
            result->action = INPUT_ACTION_RETURN_TO_BROWSER;
        } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
            new_vol += 0.04f;  // 5% display step
            if (new_vol > 1.00f) new_vol = 1.00f;  // Cap at 100%
            result->action = INPUT_ACTION_SET_VOLUME;
            result->data.set_volume.volume = new_vol;
        } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            new_vol -= 0.04f;  // 5% display step
            if (new_vol < 0.20f) new_vol = 0.20f;  // Cap at 20%
            result->action = INPUT_ACTION_SET_VOLUME;
            result->data.set_volume.volume = new_vol;
        }
    }

    return ESP_OK;
}

esp_err_t ui_input_handle_scancode(bsp_input_scancode_t scancode,
                                    input_handler_context_t *ctx,
                                    input_action_result_t *result) {
    if (!ctx || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    // Handle F6 key (0x40) for exit
    if (scancode == 0x40) {
        if (*ctx->browser_active) {
            result->action = INPUT_ACTION_EXIT_TO_LAUNCHER;
        } else {
            // During playback - return to file browser
            result->action = INPUT_ACTION_RETURN_TO_BROWSER;
        }
        return ESP_OK;
    }

    // Handle F4 key (0x3E) for channel page switching during playback
    if (!*ctx->browser_active && ctx->playing && scancode == 0x3E) {
        *ctx->tab_pressed = true;  // Set flag for rendering
        return ESP_OK;
    }

    // Handle F3 key (0x3D) for view mode toggle during playback
    if (!*ctx->browser_active && ctx->playing && scancode == 0x3D) {
        *ctx->view_key_pressed = true;  // Set flag for rendering
        return ESP_OK;
    }

    // Handle Space bar (0x39) for pause/resume during playback
    if (!*ctx->browser_active && ctx->playing && scancode == 0x39) {
        result->action = INPUT_ACTION_PAUSE_RESUME;
        return ESP_OK;
    }

    // Handle number keys (0-9) for channel mute toggle during playback
    // Standard PC scancodes (Set 1): 0=0x0B, 1-9=0x02-0x0A
    if (!*ctx->browser_active && ctx->playing) {
        int channel = -1;
        if (scancode >= 0x02 && scancode <= 0x0A) {
            // Keys 1-9: scancodes 0x02-0x0A map directly to channels 1-9
            channel = scancode - 0x01;
        } else if (scancode == 0x0B) {
            // Key 0: scancode 0x0B maps to channel 0
            channel = 0;
        }

        if (channel >= 0) {
            result->action = INPUT_ACTION_TOGGLE_CHANNEL_MUTE;
            result->data.toggle_mute.channel = channel;
            return ESP_OK;
        }
    }

    // Handle arrow keys via scancode (fallback for navigation)
    if (*ctx->browser_active) {
        if (scancode == 0x48) {  // Up arrow
            file_browser_up(ctx->browser);
            result->action = INPUT_ACTION_REDRAW_BROWSER;
        } else if (scancode == 0x50) {  // Down arrow
            file_browser_down(ctx->browser);
            result->action = INPUT_ACTION_REDRAW_BROWSER;
        } else if (scancode == 0x4B) {  // Left arrow
            if (file_browser_back(ctx->browser) == ESP_OK) {
                result->action = INPUT_ACTION_REDRAW_BROWSER;
            }
        } else if (scancode == 0x4D || scancode == 0x1C) {  // Right arrow or Enter
            // Same logic as navigation RIGHT
            bool is_directory = false;
            if (ctx->browser->selected_index < ctx->browser->count) {
                is_directory = ctx->browser->files[ctx->browser->selected_index].is_dir;
            }

            char selected_path[256];
            esp_err_t select_res = file_browser_select(ctx->browser, selected_path, sizeof(selected_path));

            if (select_res == ESP_OK && !is_directory) {
                // File selected - check if it's a supported music file
                const char *ext = strrchr(selected_path, '.');
                if (ext && (strcasecmp(ext, ".mod") == 0 || strcasecmp(ext, ".xm") == 0 ||
                            strcasecmp(ext, ".s3m") == 0 || strcasecmp(ext, ".it") == 0 ||
                            strcasecmp(ext, ".vgm") == 0 || strcasecmp(ext, ".vgz") == 0)) {
                    // Valid music file - request load
                    result->action = INPUT_ACTION_LOAD_MOD_FILE;
                    strncpy(result->data.load_mod.path, selected_path, sizeof(result->data.load_mod.path) - 1);
                    result->data.load_mod.path[sizeof(result->data.load_mod.path) - 1] = '\0';
                }
            } else if (select_res == ESP_OK) {
                // Directory entered
                result->action = INPUT_ACTION_REDRAW_BROWSER;
            }
        }
    }

    return ESP_OK;
}
