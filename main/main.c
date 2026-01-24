#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "mod_player.h"
#include "mod_backend_config.h"  // For MOD_CONFIG_SAMPLE_RATE
#include "sdcard.h"
#include "file_browser.h"
#include "xmp_compat.h"  // For xmp_frame_info, xmp_channel_info, xmp_event (structure definitions)
#include "mod_backend.h"  // For MOD_MAX_CHANNELS and unified backend interface
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "custom_certificates.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "simple_font.h"  // FONT_HEIGHT defined here
#include "esp_heap_caps.h"
#include "portmacro.h"
#include "driver/ppa.h"  // For PPA rotation
#include "hal/color_types.h"  // For color_pixel_argb8888_data_t
#include "esp_attr.h"  // For IRAM_ATTR
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "profiling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ui_theme.h"
#include "ui_primitives.h"
#include "ui_icons.h"
#include "spectrum_analyzer.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "fkey_icons.h"
#include "app_state.h"
#include "graphics/hw_accel.h"
#include "ui/ui_file_browser.h"
#include "ui/ui_input_handler.h"

// VGM player support (always enabled)
#include "vgm_player.h"

// Constants
static char const TAG[] = "main";

// Internal flash wear levelling handle
static wl_handle_t int_flash_wl_handle = WL_INVALID_HANDLE;

// Framebuffer dimensions (use module constants)
#define FB_WIDTH  HW_ACCEL_FB_WIDTH
#define FB_HEIGHT HW_ACCEL_FB_HEIGHT
#define MARGIN_LEFT   APP_STATE_MARGIN_LEFT
#define MARGIN_RIGHT  APP_STATE_MARGIN_RIGHT
#define MARGIN_TOP    APP_STATE_MARGIN_TOP
#define MARGIN_BOTTOM APP_STATE_MARGIN_BOTTOM
#define CONTENT_WIDTH  APP_STATE_CONTENT_WIDTH
#define CONTENT_HEIGHT APP_STATE_CONTENT_HEIGHT

// Format: ARGB32 = 0xAARRGGBB, RGB565 = 0bRRRRRGGGGGGBBBBB
static inline uint16_t argb32_to_rgb565(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Application state (managed by app_state module)
static app_state_t *state = NULL;

// Legacy compatibility
#define VU_DECAY_RATE       APP_STATE_VU_DECAY_RATE
#define VU_ATTACK_RATE      APP_STATE_VU_ATTACK_RATE
#define VU_PEAK_DECAY_RATE  APP_STATE_VU_PEAK_DECAY_RATE
#define VU_PEAK_HOLD_FRAMES APP_STATE_VU_PEAK_HOLD_FRAMES
#define MAX_MOD_FILE_SIZE   APP_STATE_MAX_MOD_FILE_SIZE
#define MAX_TRACKER_ROWS    APP_STATE_MAX_TRACKER_ROWS

// Convenience accessors
#define CURRENT_FB (state->fb)

#define RGB565_BLACK   0x0000
#define RGB565_WHITE   0xFFFF
#define RGB565_RED     0xF800
#define RGB565_GREEN   0x07E0
#define RGB565_BLUE    0x001F
#define RGB565_YELLOW  0xFFE0
#define RGB565_MAGENTA 0xF81F
#define RGB565_CYAN    0x07FF
#define RGB565_GRAY    0x8410
#define RGB565_DARK_BLUE 0x1084

// Wrapper functions to redirect to new modules
// These allow the existing code in main.c to continue working without changes
static inline void ppa_fill_framebuffer(uint16_t *fb_ptr, int width, int height, uint16_t color) {
    hw_accel_fill_framebuffer(fb_ptr, width, height, color);
}

static inline void ppa_fill_rect(uint16_t *fb_ptr, int width, int height, int x, int y, int w, int h, uint16_t color) {
    hw_accel_fill_rect(fb_ptr, width, height, x, y, w, h, color);
}

static inline void scroll_framebuffer_ppa(uint16_t *fb_pixels, int src_y, int dst_y, int copy_height, int stride) {
    hw_accel_scroll_framebuffer(fb_pixels, src_y, dst_y, copy_height, stride);
}

static inline void blit(int row_y, int row_height) {
    hw_accel_blit(NULL, state->fb, state->fb_rotated, row_y, row_height);
}

static inline void draw_file_browser(file_browser_t *browser) {
    ui_draw_file_browser(state->fb, FB_WIDTH, FB_HEIGHT, browser);
}

static inline void format_channel_string(char *ch_str, size_t ch_str_size,
                                         unsigned char note, unsigned char ins,
                                         unsigned char fxt, unsigned char fxp,
                                         bool use_compact, const char *note_names[12]) {
    ui_format_channel_string(ch_str, ch_str_size, note, ins, fxt, fxp, use_compact, note_names);
}

// Hardware-accelerated scrolling using PPA SRM with 0° rotation
// Optimized: Direct copy from source to destination (no temp buffer needed)
// Since src_y > dst_y (scrolling up), regions don't overlap, so we can copy directly

// Blit function - rotates framebuffer and sends to display via BSP
// BSP handles vsync and double buffering internally


void app_main(void) {
    // Initialize application state
    state = app_state_init();
    if (!state) {
        ESP_LOGE(TAG, "Failed to initialize application state");
        return;
    }

    // Start the GPIO interrupt service
    gpio_install_isr_service(0);

    // Initialize the Non Volatile Storage service
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    // Initialize the Board Support Package
    // Use double buffering (num_fbs = 2) - BSP handles vsync and buffer swapping internally
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
                .num_fbs                = 2,  // BSP handles double buffering and vsync
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));

    // Initialize hardware acceleration module
    ESP_ERROR_CHECK(hw_accel_init());
    state->ppa_srm_handle = hw_accel_get_srm_handle();
    state->ppa_fill_handle = hw_accel_get_fill_handle();

    // Allocate single logical landscape framebuffer (800x480) from DMA-capable PSRAM
    // Use 64-byte alignment for L2 cache line optimization (ESP32-P4 has 64-byte cache lines)
    size_t fb_size = FB_WIDTH * FB_HEIGHT * sizeof(uint16_t);  // 800x480x2 = 768000 bytes
    state->fb = (uint16_t*)heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (state->fb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate logical framebuffer from DMA-capable PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Allocated logical framebuffer: %zu bytes (800x480)", fb_size);

    // Allocate single rotated framebuffer (480x800) for PPA output
    // Use 64-byte alignment for L2 cache line optimization
    size_t fb_rotated_size = 480 * 800 * sizeof(uint16_t);  // 480x800x2 = 768000 bytes
    state->fb_rotated = (uint16_t*)heap_caps_aligned_alloc(64, fb_rotated_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (state->fb_rotated == NULL) {
        ESP_LOGE(TAG, "Failed to allocate rotated framebuffer from DMA-capable PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Allocated rotated framebuffer: %zu bytes (480x800)", fb_rotated_size);

    // Initialize framebuffer to black
    ppa_fill_framebuffer(state->fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    ESP_LOGI(TAG, "Initialized framebuffer to black");

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&state->input_event_queue));

    // Initialize audio system
    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_WHITE, 2, "Initializing audio...");
    blit(-1, 0);  // Rotate and send to display via BSP (handles vsync and double buffering)
    res = audio_init();
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Audio initialized successfully");
        
        // Diagnose ES8156 codec configuration
        audio_diagnose_es8156();
        
        // Initialize MOD player first (task will start but won't play until MOD is loaded)
        res = mod_player_init(MOD_CONFIG_SAMPLE_RATE);

        // Small delay to let MOD player task initialize
        vTaskDelay(pdMS_TO_TICKS(50));

        // Startup beep removed
        // audio_beep(100);
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "MOD player initialized");
        } else {
            ESP_LOGW(TAG, "MOD player initialization failed: %s", esp_err_to_name(res));
        }

        // Initialize VGM player
        res = vgm_player_init(VGM_PLAYBACK_SAMPLE_RATE);
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "VGM player initialized");
        } else {
            ESP_LOGW(TAG, "VGM player initialization failed: %s", esp_err_to_name(res));
        }
    } else {
        ESP_LOGW(TAG, "Audio initialization failed: %s", esp_err_to_name(res));
        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Audio init failed");
        blit(-1, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Skip WiFi initialization for faster startup (can re-enable later if needed)
    //
    // if (wifi_remote_initialize() == ESP_OK) {
    //
    //     ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //     pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Starting WiFi stack...");
    //     blit(-1, 0);
    //     wifi_connection_init_stack();  // Start the Espressif WiFi stack
    //
    //     ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //     pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Connecting to WiFi network...");
    //     blit(-1, 0);
    //
    //     if (wifi_connect_try_all() == ESP_OK) {
    //         ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //         pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Succesfully connected to WiFi network");
    //         blit(-1, 0);
    //     } else {
    //         pax_background(&fb, RED);
    //         pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "Failed to connect to WiFi network");
    //         blit(-1, 0);
    //     }
    // } else {
    //     bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    //     ESP_LOGE(TAG, "WiFi radio not responding, WiFi not available");
    //     pax_background(&fb, RED);
    //     pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "WiFi unavailable");
    //     blit(-1, 0);
    // }
    //
    // vTaskDelay(pdMS_TO_TICKS(500));

    // Initialize SD card
    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_BLACK, 2, "Initializing SD card...");
    blit(-1, 0);
    res = sdcard_init();
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s", esp_err_to_name(res));
        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "SD card init failed");
        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 18, RGB565_BLACK, 2, "Continuing without SD");
        blit(-1, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Mount internal flash FAT filesystem for icons
    {
        esp_vfs_fat_mount_config_t fat_mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
            .disk_status_check_enable = false,
            .use_one_fat = false,
        };
        esp_err_t fat_res = esp_vfs_fat_spiflash_mount_rw_wl("/int", "locfd", &fat_mount_config, &int_flash_wl_handle);
        if (fat_res != ESP_OK) {
            ESP_LOGW(TAG, "Failed to mount internal flash: %s", esp_err_to_name(fat_res));
            // Continue anyway - icons won't be available
        } else {
            ESP_LOGI(TAG, "Internal flash mounted at /int");
            // Initialize F-key icons (uses PSRAM for stb_image allocations)
            fkey_icons_init();
        }
    }

    // Main section of the app

    // This example shows how to read from the BSP event queue to read input events

    // If you want to run something at an interval in this same main thread you can replace portMAX_DELAY with an amount
    // of ticks to wait, for example pdMS_TO_TICKS(1000)

    // File browser state (static to avoid stack overflow)
    // Channel colors (RGB565)
    static const uint16_t channel_colors_rgb565[MOD_MAX_CHANNELS] = {
        0xF800, 0x07E0, 0x001F, 0xFFE0,  // Red, Green, Blue, Yellow
        0xF81F, 0x07FF, 0xFC00, 0x87E0,  // Magenta, Cyan, Orange, Lime
        0x041F, 0xF810, 0x87FF, 0xFC10,  // Light Blue, Pink, Light Cyan, Light Red
        0x87F0, 0x841F, 0xFFF0, 0x87FF,  // Light Green, Light Blue, Light Yellow, etc.
        0xFC1F, 0x87FF, 0xFFF0, 0x8410,  // More colors...
        0xFC10, 0x87F0, 0x841F, 0xFC10,  // Repeating pattern for more channels
        0x87F0, 0x841F, 0xFFF0, 0x87FF,
        0xFC1F, 0x87FF, 0xFFF0, 0x8410,
        0xFC10, 0x87F0, 0x841F, 0xFC10,
        0x87F0, 0x841F, 0xFFF0, 0x87FF,
        0xFC1F, 0x87FF, 0xFFF0, 0x8410,
        0xFC10, 0x87F0, 0x841F, 0xFC10,
        0x87F0, 0x841F, 0xFFF0, 0x87FF,
        0xFC1F, 0x87FF, 0xFFF0, 0x8410,
    };

    // Start with file browser if SD card is mounted
    // NOTE: Don't call blit() here - the main loop will render and call blit() once per frame
    if (sdcard_is_mounted()) {
        state->browser_active = true;
        file_browser_init(&state->browser, "/sdcard");
        // Initial file browser will be drawn in main loop when browser_rendered is false
    } else {
        // No SD card - show error (will be drawn in main loop)
        state->browser_active = false;  // Don't show browser if no SD card
    }

    while (1) {
        PROFILING_START(frame);
        
        // Track if we need to render this frame
        static bool needs_render = false;
        bool did_render = false;  // Track if we actually rendered anything this frame
        
        // Check input queue FIRST with ZERO timeout for instant response (non-blocking)
        bsp_input_event_t event;
        
        // Set up input handler context
        bool is_playing = mod_player_is_playing();
        bool is_paused = mod_player_is_paused();
        is_playing = is_playing || vgm_player_is_playing();
        is_paused = is_paused || vgm_player_is_paused();
        input_handler_context_t input_ctx = {
            .browser = &state->browser,
            .browser_active = &state->browser_active,
            .playing = is_playing,
            .paused = is_paused,
            .current_volume = 0.0f,
            .current_view = state->current_view,
            .tab_pressed = &state->tab_pressed_this_frame,
            .view_key_pressed = &state->view_key_pressed,
        };
        audio_get_volume(&input_ctx.current_volume);
        
        // Process ALL pending input events before doing anything else
        while (xQueueReceive(state->input_event_queue, &event, 0) == pdTRUE) {
            input_action_result_t action_result;
            
            // Process input event
            if (ui_input_handle_event(&event, &input_ctx, &action_result) == ESP_OK) {
                // Execute action based on result
                switch (action_result.action) {
                    case INPUT_ACTION_REDRAW_BROWSER:
                        needs_render = true;
                        break;
                        
                    case INPUT_ACTION_LOAD_MOD_FILE: {
                        // Load music file (MOD or VGM)
                        ESP_LOGI("main", "INPUT_ACTION_LOAD_MOD_FILE: %s", action_result.data.load_mod.path);
                        FILE *f = fopen(action_result.data.load_mod.path, "rb");
                        if (!f) {
                            ESP_LOGE("main", "Failed to open file: %s", action_result.data.load_mod.path);
                            break;
                        }
                        ESP_LOGI("main", "File opened successfully");
                        {
                            fseek(f, 0, SEEK_END);
                            long file_size = ftell(f);
                            fseek(f, 0, SEEK_SET);

                            // Reject files larger than 10MB
                            if (file_size > MAX_MOD_FILE_SIZE) {
                                fclose(f);
                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "File too large (>10MB)");
                                blit(-1, 0);
                                break;
                            }

                            // Check file extension to determine player type
                            const char *ext = strrchr(action_result.data.load_mod.path, '.');
                            bool is_vgm = (ext && (strcasecmp(ext, ".vgm") == 0 || strcasecmp(ext, ".vgz") == 0));
                            ESP_LOGI("main", "File size: %ld, ext: %s, is_vgm: %d", file_size, ext ? ext : "NULL", is_vgm);

                            // Free previous MOD data if any
                            if (state->mod_file.data) {
                                free(state->mod_file.data);
                            }

                            // Allocate file buffer in PSRAM
                            state->mod_file.data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                            if (state->mod_file.data) {
                                size_t read = fread(state->mod_file.data, 1, file_size, f);
                                fclose(f);

                                if (read == (size_t)file_size) {
                                    state->mod_file.size = file_size;
                                    strncpy(state->mod_file.path, action_result.data.load_mod.path, sizeof(state->mod_file.path) - 1);
                                    state->mod_file.path[sizeof(state->mod_file.path) - 1] = '\0';

                                    // Show loading message
                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                          (FB_WIDTH - 10 * FONT_WIDTH * 2) / 2,
                                                          (FB_HEIGHT - FONT_HEIGHT * 2) / 2,
                                                          RGB565_WHITE, 2, "Loading...");
                                    blit(-1, 0);

                                    if (is_vgm) {
                                        ESP_LOGI("main", "Loading VGM file...");
                                        // Stop MOD player if playing
                                        if (mod_player_is_playing()) {
                                            mod_player_stop();
                                        }

                                        // Load and start VGM file
                                        res = vgm_player_load(state->mod_file.data, state->mod_file.size);
                                        ESP_LOGI("main", "vgm_player_load returned: %d", res);
                                        if (res == ESP_OK) {
                                            res = vgm_player_start();
                                            ESP_LOGI("main", "vgm_player_start returned: %d", res);
                                            if (res == ESP_OK) {
                                                state->browser_active = false;
                                                state->current_view = APP_VIEW_VGM;
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                                                ESP_LOGI("main", "VGM playback started, switched to VGM view");
                                            } else {
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start VGM");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            free(state->mod_file.data);
                                            state->mod_file.data = NULL;
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load VGM");
                                            blit(-1, 0);
                                        }
                                    } else
                                    {
                                        // Stop VGM player if playing
                                        if (vgm_player_is_playing()) {
                                            vgm_player_stop();
                                        }

                                        // Load and start MOD file
                                        res = mod_player_load(state->mod_file.data, state->mod_file.size);
                                        if (res == ESP_OK) {
                                            res = mod_player_start();
                                            if (res == ESP_OK) {
                                                state->browser_active = false;
                                                state->tracker.mod_info_loaded = false;
                                                state->current_view = APP_VIEW_TRACKER;

                                                // Reset VU meter levels
                                                for (int i = 0; i < MOD_MAX_CHANNELS; i++) {
                                                    state->vu_state.levels[i] = 0.0f;
                                                    state->vu_state.peaks[i] = 0.0f;
                                                    state->vu_state.peak_hold[i] = 0;
                                                }

                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                                            } else {
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            free(state->mod_file.data);
                                            state->mod_file.data = NULL;
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                            blit(-1, 0);
                                        }
                                    }
                                } else {
                                    free(state->mod_file.data);
                                    state->mod_file.data = NULL;
                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                    blit(-1, 0);
                                }
                            } else {
                                fclose(f);
                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of PSRAM");
                                blit(-1, 0);
                            }
                        }
                        break;
                    }
                    
                    case INPUT_ACTION_EXIT_TO_LAUNCHER:
                        if (mod_player_is_playing()) {
                            mod_player_stop();
                        }
                        if (vgm_player_is_playing()) {
                            vgm_player_stop();
                        }
                        audio_stop();
                        audio_set_volume(0.0f);
                        vTaskDelay(pdMS_TO_TICKS(50));
                        bsp_device_restart_to_launcher();
                        break;

                    case INPUT_ACTION_RETURN_TO_BROWSER:
                        mod_player_stop();
                        vgm_player_stop();
                        state->browser_active = true;
                        file_browser_refresh(&state->browser);
                        needs_render = true;
                        break;
                        
                    case INPUT_ACTION_SET_VOLUME:
                        audio_set_volume(action_result.data.set_volume.volume);
                        break;
                        
                    case INPUT_ACTION_PAUSE_RESUME:
                        if (state->current_view == APP_VIEW_VGM) {
                            // VGM pause/resume
                            if (vgm_player_is_paused()) {
                                vgm_player_resume();
                            } else {
                                vgm_player_pause();
                            }
                        } else
                        {
                            // MOD pause/resume
                            if (mod_player_is_paused()) {
                                mod_player_resume();
                            } else {
                                mod_player_pause();
                            }
                        }
                        break;
                        
                    case INPUT_ACTION_TOGGLE_CHANNEL_MUTE:
                        mod_player_toggle_channel_mute(action_result.data.toggle_mute.channel);
                        break;
                        
                    case INPUT_ACTION_NONE:
                    default:
                        break;
                }
            }
        }  // End of input event processing (while loop for xQueueReceive)
        
        // Render file browser if needed (input handlers set needs_render flag)
        // Also render on first loop if browser is active but hasn't been rendered yet
        static bool browser_rendered = false;
        if (state->browser_active && (needs_render || !browser_rendered)) {
            draw_file_browser(&state->browser);
            needs_render = false;  // Clear flag after rendering
            browser_rendered = true;  // Mark as rendered
            did_render = true;  // Mark that we rendered this frame
        } else if (!state->browser_active) {
            browser_rendered = false;  // Reset when browser becomes inactive
        }
        
        // Render "no SD card" error if needed (only once, or if it changed)
        static bool error_rendered = false;
        static bool last_sdcard_mounted = false;
        bool current_sdcard_mounted = sdcard_is_mounted();
        if (!state->browser_active && !mod_player_is_playing() && !current_sdcard_mounted && (!error_rendered || last_sdcard_mounted != current_sdcard_mounted)) {
            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "SD card not mounted");
            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 18, RGB565_WHITE, 2, "Insert SD card and");
            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 36, RGB565_WHITE, 2, "restart device");
            error_rendered = true;
            did_render = true;  // Mark that we rendered this frame
        } else if (current_sdcard_mounted && !last_sdcard_mounted) {
            error_rendered = false;  // Reset if SD card is mounted
        }
        last_sdcard_mounted = current_sdcard_mounted;
        
        // Render playback UI if playing (MOD or VGM)
        // For smooth scrolling, we need to render every frame, not just on row changes
        bool any_player_active = mod_player_is_playing();
        any_player_active = any_player_active || vgm_player_is_playing();
        if (any_player_active && !state->browser_active) {
            // Track mode transitions - clear framebuffer when switching from browser to playback
            static bool last_browser_active = true;
            if (last_browser_active && !state->browser_active) {
                // Switching from browser to playback - clear framebuffer for clean transition
                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                did_render = true;  // Ensure we blit to show the cleared screen
            }
            last_browser_active = state->browser_active;
            
            PROFILING_START(render);
            struct xmp_frame_info frame_info;
            if (mod_player_get_frame_info(&frame_info) == ESP_OK) {
                // Always render to support smooth scrolling (updates every frame during scroll)
                // Row change detection is handled inside the render logic
                {
                    // Load module info if not loaded
                    static bool tracker_initialized = false;  // Declare here so it persists across renders
                    if (!state->tracker.mod_info_loaded) {
                        mod_player_get_module_info(&state->tracker.mod_info);
                        state->tracker.mod_info_loaded = true;
                        // Reset tracker_initialized when module info is reloaded (new MOD started)
                        tracker_initialized = false;
                        state->tracker.tick_history_count = 0;  // Reset tick history for new MOD
                        state->tracker.last_row = -1;  // Reset state->tracker.last_row to force first render
                        state->channel_page_offset = 0;  // Reset to first page when loading new MOD
                    }
                    
                    int num_channels = state->tracker.mod_info.mod ? state->tracker.mod_info.mod->chn : 0;
                    if (num_channels > 0 && num_channels <= MOD_MAX_CHANNELS) {
                        // Handle view mode toggle ('V' key)
                        if (state->view_key_pressed) {
                            // Only cycle through MOD views if not playing VGM
                            if (state->current_view != APP_VIEW_VGM) {
                                state->current_view = (app_playback_view_t)((state->current_view + 1) % 3);  // Cycle through APP_VIEW_TRACKER, APP_VIEW_SPECTRUM, APP_VIEW_INFO
                            }
                            state->view_key_pressed = false;
                            // Clear framebuffer when switching views
                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                        }

                        // Update VU meter decay for smooth animation
                        for (int ch = 0; ch < num_channels; ch++) {
                            // Get current volume from frame info
                            float target_level = (float)frame_info.channel_info[ch].volume / 64.0f;

                            // Smoothed attack and decay to reduce flicker
                            if (target_level > state->vu_state.levels[ch]) {
                                // Smoothed attack - lerp toward target
                                state->vu_state.levels[ch] += (target_level - state->vu_state.levels[ch]) * VU_ATTACK_RATE;
                            } else {
                                // Slow decay
                                state->vu_state.levels[ch] *= (1.0f - VU_DECAY_RATE);
                                if (state->vu_state.levels[ch] < 0.01f) {
                                    state->vu_state.levels[ch] = 0.0f;
                                }
                            }

                            // Peak hold and decay
                            if (target_level > state->vu_state.peaks[ch]) {
                                state->vu_state.peaks[ch] = target_level;
                                state->vu_state.peak_hold[ch] = VU_PEAK_HOLD_FRAMES;
                            } else if (state->vu_state.peak_hold[ch] > 0) {
                                state->vu_state.peak_hold[ch]--;
                            } else {
                                state->vu_state.peaks[ch] *= (1.0f - VU_PEAK_DECAY_RATE);
                                if (state->vu_state.peaks[ch] < 0.01f) {
                                    state->vu_state.peaks[ch] = 0.0f;
                                }
                            }
                        }

                        // Channel pagination: Show 4 channels per page, switch pages with Tab
                        // Check for Tab key press flag (set by scancode handler)
                        if (state->tab_pressed_this_frame) {
                            // Cycle to next page (4 channels per page)
                            int max_pages = (num_channels + 3) / 4;  // Round up
                            state->channel_page_offset = (state->channel_page_offset + 4) % (max_pages * 4);
                            if (state->channel_page_offset >= num_channels) {
                                state->channel_page_offset = 0;  // Wrap around
                            }
                            state->tab_pressed_this_frame = false;  // Reset flag
                        }
                        
                        // Calculate which channels to display (up to 4 channels per page)
                        int channels_per_page = 4;
                        int start_channel = state->channel_page_offset;
                        int end_channel = start_channel + channels_per_page;
                        if (end_channel > num_channels) {
                            end_channel = num_channels;
                        }
                        int visible_channels = end_channel - start_channel;

                        // Update global VU meter layout cache
                        state->vu_state.start_channel = start_channel;
                        state->vu_state.visible_channels = visible_channels;

                        // Calculate layout (cache for performance)
                        static int cached_line_height = 0;
                        static int cached_max_rows = 0;
                        static int cached_ch_width = 0;
                        static int cached_num_channels = 0;
                        
                        // Recalculate if channel count changed
                        // Use channels_per_page (4) for layout since we paginate channel display
                        int layout_channels = (visible_channels < channels_per_page) ? visible_channels : channels_per_page;
                        if (cached_num_channels != layout_channels) {
                            // Dynamic text scaling to use ~85% of content width (after margins: 780px)
                            const int content_width = CONTENT_WIDTH;  // 780 (800 - 10 - 10)
                            const int content_height = CONTENT_HEIGHT;  // 470 (480 - 5 - 5)
                            int available_width = (int)(content_width * 0.85);
                            int ch_width_estimate = available_width / layout_channels;
                            // Scale line height from 16px to 32px based on channel width (8x16 font needs larger range)
                            // This ensures font_scale of at least 1 (16px) up to 2 (32px) for 8x16 font
                            int estimated_line_height = 16 + (ch_width_estimate - 40) * 16 / 200;
                            if (estimated_line_height < 16) estimated_line_height = 16;
                            if (estimated_line_height > 32) estimated_line_height = 32;

                            // Calculate font_scale based on estimated line_height
                            int font_scale = (estimated_line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                            if (font_scale < 1) font_scale = 1;
                            if (font_scale > 3) font_scale = 3;

                            // Actual rendered text height
                            int actual_text_height = font_scale * FONT_HEIGHT;

                            // Ensure line_height is at least as tall as the rendered text to prevent overlap
                            cached_line_height = (estimated_line_height > actual_text_height) ? estimated_line_height : actual_text_height;

                            cached_max_rows = (int)(content_height / cached_line_height);
                            cached_ch_width = ch_width_estimate;
                            cached_num_channels = layout_channels;
                        }
                        
                        int line_height = cached_line_height;
                        int max_rows_on_screen = cached_max_rows;
                        int ch_width = cached_ch_width;

                        // Update global VU line height cache
                        state->vu_state.cached_line_height = line_height;

                        // Header height: use smaller header to maximize tracker content area
                        // Font scale 1 = 16px text height, add small padding
                        const int header_height = FONT_HEIGHT + 4;  // shared header height
                        int header_y = MARGIN_TOP + header_height;  // First row starts immediately after header
                        
                        // Track volume changes to update header
                        static float last_volume = -1.0f;
                        float current_vol = 0.0f;
                        bool vol_changed = false;
                        if (audio_get_volume(&current_vol) == ESP_OK) {
                            vol_changed = (current_vol != last_volume);
                            if (vol_changed) {
                                last_volume = current_vol;
                            }
                        }
                        
                        // Get direct framebuffer access for scrolling (RGB565 = 2 bytes per pixel)
                        // Logical landscape framebuffer: 800 pixels wide, 480 pixels tall
                        uint16_t *fb_pixels = CURRENT_FB;  // Direct framebuffer access
                        const int logical_width = FB_WIDTH;  // 800
                        const int logical_height = FB_HEIGHT;  // 480
                        int stride = logical_width;  // Pixels per row in landscape framebuffer (800)
                        
                        // Hex digit lookup table removed - using snprintf with %02X format instead
                        
                        // Note name lookup
                        static const char *note_names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
                        
                        // Determine if we need compact format based on estimated text width
                        // Full format: "C-4 01 0F02" = 11 chars, Compact: "C4 01 0F02" = 10 chars
                        // Separator: " " = 1 char
                        // Estimate: full format needs 12 chars per channel, compact needs 11 chars per channel
                        // Calculate font_scale first
                        int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                        if (font_scale < 1) font_scale = 1;
                        if (font_scale > 3) font_scale = 3;
                        int estimated_chars_per_channel_full = 12;  // "C-4 01 0F02 "
                        int estimated_chars_per_channel_compact = 11;  // "C4 01 0F02 "
                        // Use channels_per_page (4) instead of num_channels for width calculation
                        // since we only show 4 channels at a time with pagination
                        int channels_for_width = (visible_channels < 4) ? visible_channels : 4;
                        int estimated_total_width_full = estimated_chars_per_channel_full * channels_for_width * FONT_WIDTH * font_scale;
                        int estimated_total_width_compact = estimated_chars_per_channel_compact * channels_for_width * FONT_WIDTH * font_scale;
                        int available_width_pixels = CONTENT_WIDTH;  // 780 pixels
                        bool use_compact_format = (estimated_total_width_full > available_width_pixels);
                        
                        // Calculate center Y position for current row (middle of visible area)
                        // Reserve line_height at bottom for VU meters
                        int scrollable_height = FB_HEIGHT - header_y - MARGIN_BOTTOM - line_height;
                        int center_y = header_y + scrollable_height / 2 - line_height / 2;  // Center vertically, accounting for line_height
                        
                        if (!tracker_initialized) {
                            // First render - draw current row at center with highlight
                            // First render - full screen (fb_fill already clears all margins)
                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                            
                            // Draw header on first render (always clear to ensure exact alignment)
                            {
                                // Clear header area with theme background
                                ui_draw_vgradient(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                  0, MARGIN_TOP, FB_WIDTH, header_height,
                                                  THEME_BG_HEADER, THEME_BG_PRIMARY);

                                // Use module metadata name instead of filename
                                const char *mod_name = (state->tracker.mod_info.mod && state->tracker.mod_info.mod->name[0])
                                                       ? state->tracker.mod_info.mod->name : "Unknown";

                                // Draw song title on the left (font scale 1 for compact header)
                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                        MARGIN_LEFT, MARGIN_TOP + 2,
                                                        THEME_TEXT_PRIMARY, 1, mod_name);

                                // Draw volume right-aligned
                                // Map actual volume (20%-100%) to display (0%-100%)
                                float header_vol = 0.0f;
                                if (audio_get_volume(&header_vol) == ESP_OK) {
                                    char vol_text[16];
                                    int vol_percent = (int)((header_vol - 0.20f) / 0.80f * 100.0f + 0.5f);
                                    if (vol_percent < 0) vol_percent = 0;
                                    if (vol_percent > 100) vol_percent = 100;
                                    snprintf(vol_text, sizeof(vol_text), "Vol: %d%%", vol_percent);
                                    int vol_width = strlen(vol_text) * FONT_WIDTH;  // font_scale = 1
                                    int vol_x = FB_WIDTH - MARGIN_RIGHT - vol_width;
                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                            vol_x, MARGIN_TOP + 2,
                                                            THEME_TEXT_PRIMARY, 1, vol_text);
                                }
                            }
                            
                            // Initialize current tick with first frame data
                            memcpy(state->tracker.current_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                            state->tracker.current_tick.pos = frame_info.pos;
                            state->tracker.current_tick.pattern = frame_info.pattern;
                            state->tracker.current_tick.row = frame_info.row;
                            state->tracker.current_tick.num_channels = num_channels;
                            state->tracker.current_tick_valid = true;
                            
                            // Draw current row at center with highlight background
                            int y = center_y;
                            
                            // Draw highlight background for current row (full width of content area)
                            int highlight_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                            ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, y, highlight_width, line_height, RGB565_DARK_BLUE);
                            
                            // Draw current frame data at center (from state->tracker.current_tick) - with pagination and centering
                            {
                                int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                if (font_scale < 1) font_scale = 1;
                                if (font_scale > 3) font_scale = 3;
                                
                                // First pass: calculate total width of visible channels
                                int total_width = 0;
                                int channel_widths[4];  // Max 4 channels per page
                                const char *separator = " ";
                                int separator_width = strlen(separator) * FONT_WIDTH * font_scale;
                                
                                for (int i = 0; i < visible_channels; i++) {
                                    int ch = start_channel + i;
                                    struct xmp_channel_info *ci = &frame_info.channel_info[ch];
                                    char ch_str[64];
                                    format_channel_string(ch_str, sizeof(ch_str), 
                                                         ci->event.note, ci->event.ins, 
                                                         ci->event.fxt, ci->event.fxp,
                                                         use_compact_format, note_names);
                                    channel_widths[i] = strlen(ch_str) * FONT_WIDTH * font_scale;
                                    total_width += channel_widths[i];
                                    // Add separator width between channels (not after last)
                                    if (i < visible_channels - 1) {
                                        total_width += separator_width;
                                    }
                                }
                                
                                // Calculate starting X position to center the channels
                                int content_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                                int start_x = MARGIN_LEFT + (content_width - total_width) / 2;
                                
                                // Second pass: render channels centered
                                int x = start_x;
                                for (int i = 0; i < visible_channels; i++) {
                                    int ch = start_channel + i;
                                    struct xmp_channel_info *ci = &frame_info.channel_info[ch];
                                    char ch_str[64];
                                    format_channel_string(ch_str, sizeof(ch_str), 
                                                         ci->event.note, ci->event.ins, 
                                                         ci->event.fxt, ci->event.fxp,
                                                         use_compact_format, note_names);
                                    
                                    bool channel_muted = false;
                                    mod_player_get_channel_mute(ch, &channel_muted);
                                    uint16_t ch_color = channel_muted ? THEME_TEXT_MUTED : theme_channel_color(ch);

                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                    x += channel_widths[i];
                                    
                                    // Draw separator and vertical line between channels (not after last)
                                    if (i < visible_channels - 1) {
                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, separator);
                                        int line_x = x + separator_width / 2;  // Middle of the space
                                        int line_top = header_y;  // Start from header bottom
                                        int line_bottom = logical_height - MARGIN_BOTTOM;  // End at content bottom
                                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT, line_x, line_top, 1, line_bottom - line_top, RGB565_GRAY);
                                        x += separator_width;
                                    }
                                }
                            }
                            
                            tracker_initialized = true;
                            // Initialize state->tracker.last_row to current row to prevent immediate scrolling on first frame
                            state->tracker.last_row = state->tracker.current_tick.row;
                            // Reset smooth scrolling state for clean start
                            state->tracker.smooth_scroll_offset = 0;
                            state->tracker.pending_new_row = false;
                            // Full screen blit on first render
                            blit(-1, 0);  // -1 means full screen, row_height ignored
                        } else {
                            // Subsequent renders - smooth scrolling
                            // NOTE: Rows are NOT added to history here - they're added when scroll completes
                            // This prevents duplicate rows from being added every frame
                            
                            // Always redraw header every frame to prevent smudging from scrolling rows
                            // This ensures the header area stays clean as rows scroll underneath
                            {
                                // Clear header area with theme background
                                ui_draw_vgradient(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                  0, MARGIN_TOP, FB_WIDTH, header_height,
                                                  THEME_BG_HEADER, THEME_BG_PRIMARY);

                                // Use module metadata name instead of filename
                                const char *mod_name = (state->tracker.mod_info.mod && state->tracker.mod_info.mod->name[0])
                                                       ? state->tracker.mod_info.mod->name : "Unknown";

                                // Draw song title on the left (font scale 1 for compact header)
                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                        MARGIN_LEFT, MARGIN_TOP + 2,
                                                        THEME_TEXT_PRIMARY, 1, mod_name);

                                // Draw volume right-aligned
                                // Map actual volume (20%-100%) to display (0%-100%)
                                float header_vol = 0.0f;
                                if (audio_get_volume(&header_vol) == ESP_OK) {
                                    char vol_text[16];
                                    int vol_percent = (int)((header_vol - 0.20f) / 0.80f * 100.0f + 0.5f);
                                    if (vol_percent < 0) vol_percent = 0;
                                    if (vol_percent > 100) vol_percent = 100;
                                    snprintf(vol_text, sizeof(vol_text), "Vol: %d%%", vol_percent);
                                    int vol_width = strlen(vol_text) * FONT_WIDTH;  // font_scale = 1
                                    int vol_x = FB_WIDTH - MARGIN_RIGHT - vol_width;
                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                            vol_x, MARGIN_TOP + 2,
                                                            THEME_TEXT_PRIMARY, 1, vol_text);
                                }
                            }
                            
                            // Pattern-timed smooth scrolling: scroll one line_height over the duration of one pattern row
                            // This keeps scrolling in sync with pattern playback
                            // Reserve line_height at bottom for VU meters
                            int scrollable_height = FB_HEIGHT - header_y - MARGIN_BOTTOM - line_height;
                            
                            // Track when current row started and its duration
                            static TickType_t row_start_tick = 0;
                            static float current_row_duration_ms = 0.0f;
                            
                            // Fixed grid approach: update history immediately when row changes
                            // This keeps history and future rows in sync (both update based on current position)
                            if (frame_info.row != state->tracker.last_row) {
                                // Immediately add current row to history when it changes (for fixed grid)
                                // This ensures history rows appear at the same rate as future rows disappear
                                if (state->tracker.current_tick_valid && state->tracker.last_row >= 0) {
                                    // Move previous row to history immediately
                                    if (state->tracker.tick_history_count < MAX_TRACKER_ROWS) {
                                        memcpy(state->tracker.tick_history[state->tracker.tick_history_count].channels, state->tracker.current_tick.channels, sizeof(state->tracker.current_tick.channels));
                                        state->tracker.tick_history[state->tracker.tick_history_count].pos = state->tracker.current_tick.pos;
                                        state->tracker.tick_history[state->tracker.tick_history_count].pattern = state->tracker.current_tick.pattern;
                                        state->tracker.tick_history[state->tracker.tick_history_count].row = state->tracker.current_tick.row;
                                        state->tracker.tick_history[state->tracker.tick_history_count].num_channels = state->tracker.current_tick.num_channels;
                                        state->tracker.tick_history_count++;
                                    } else {
                                        // Shift history (oldest first, so remove oldest)
                                        memmove(state->tracker.tick_history, state->tracker.tick_history + 1, (MAX_TRACKER_ROWS - 1) * sizeof(app_tracker_tick_t));
                                        memcpy(state->tracker.tick_history[MAX_TRACKER_ROWS - 1].channels, state->tracker.current_tick.channels, sizeof(state->tracker.current_tick.channels));
                                        state->tracker.tick_history[MAX_TRACKER_ROWS - 1].pos = state->tracker.current_tick.pos;
                                        state->tracker.tick_history[MAX_TRACKER_ROWS - 1].pattern = state->tracker.current_tick.pattern;
                                        state->tracker.tick_history[MAX_TRACKER_ROWS - 1].row = state->tracker.current_tick.row;
                                        state->tracker.tick_history[MAX_TRACKER_ROWS - 1].num_channels = state->tracker.current_tick.num_channels;
                                    }
                                }
                                // Update current tick with new row data immediately
                                memcpy(state->tracker.current_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                                state->tracker.current_tick.pos = frame_info.pos;
                                state->tracker.current_tick.pattern = frame_info.pattern;
                                state->tracker.current_tick.row = frame_info.row;
                                state->tracker.current_tick.num_channels = num_channels;
                                state->tracker.current_tick_valid = true;
                                
                                state->tracker.last_row = frame_info.row;
                            }
                            
                            // Check if we have a new row - allow continuous scrolling without waiting
                            if (frame_info.row != state->tracker.last_row && !state->tracker.pending_new_row) {
                                // New row detected - prepare it for smooth scrolling
                                state->tracker.pending_new_row = true;
                                memcpy(state->tracker.pending_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                                state->tracker.pending_tick.pos = frame_info.pos;
                                state->tracker.pending_tick.pattern = frame_info.pattern;
                                state->tracker.pending_tick.row = frame_info.row;
                                state->tracker.pending_tick.num_channels = num_channels;
                                
                                // Calculate row duration: speed is frames per row, frame_time is microseconds per frame
                                // Total row duration = speed * frame_time microseconds
                                float row_duration_us = (float)frame_info.speed * (float)frame_info.frame_time;
                                float single_row_duration_ms = row_duration_us / 1000.0f;  // Convert to milliseconds
                                
                                // Scroll one line_height over multiple row durations for readability
                                // Typical trackers scroll slower than row rate - use more rows for comfortable reading
                                const int ROWS_PER_SCROLL = 3;  // Scroll one line_height over this many pattern rows
                                current_row_duration_ms = single_row_duration_ms * ROWS_PER_SCROLL;
                                
                                row_start_tick = xTaskGetTickCount();  // Record when this row started
                                
                                // If we're already scrolling, continue from current offset (seamless transition)
                                // If not scrolling, start from 0
                                if (state->tracker.smooth_scroll_offset >= line_height) {
                                    state->tracker.smooth_scroll_offset = 0;  // Reset if previous row just completed
                                }
                                // Otherwise keep current offset for seamless continuous scrolling
                                
                                // Debug: log row timing info (only occasionally to avoid spam)
                                static int debug_row_count = 0;
                                if (debug_row_count++ % 20 == 0) {
                                    ESP_LOGI(TAG, "Row timing: speed=%d, frame_time=%d us, single_row=%.1f ms, scroll_duration=%.1f ms (over %d rows), line_height=%d", 
                                            frame_info.speed, frame_info.frame_time, single_row_duration_ms, 
                                            current_row_duration_ms, ROWS_PER_SCROLL, line_height);
                                }
                                
                                state->tracker.last_row = frame_info.row;  // Update to prevent repeated detection
                            }
                            
                            // Calculate scroll position based on elapsed time since row started
                            // Only scroll if we have a pending row
                            if (state->tracker.pending_new_row && current_row_duration_ms > 0.0f) {
                                TickType_t current_time = xTaskGetTickCount();
                                TickType_t elapsed_ticks = current_time - row_start_tick;
                                float elapsed_ms = (float)elapsed_ticks * (1000.0f / configTICK_RATE_HZ);
                                
                                // Calculate scroll position: 0 to line_height over row_duration_ms
                                // This ensures we scroll exactly one line_height over the duration of one pattern row
                                float scroll_progress = elapsed_ms / current_row_duration_ms;
                                if (scroll_progress > 1.0f) scroll_progress = 1.0f;  // Clamp to 1.0
                                
                                state->tracker.smooth_scroll_offset = (int)(scroll_progress * (float)line_height);
                                
                                // Note: In fixed grid mode, history is updated immediately when frame_info.row changes
                                // (handled above), so we don't need to move rows here. The state->tracker.smooth_scroll_offset
                                // is kept for potential future use but doesn't affect the fixed grid display.
                            }
                            
                            // Fixed row grid approach: calculate how many rows fit on screen
                            // Divide screen into fixed row positions, update data on row changes
                            int num_visible_rows = scrollable_height / line_height;  // How many complete rows fit
                            // Calculate center row index: reserve one row for center, split rest above/below
                            // This ensures center row is always visible and not overwritten
                            int rows_available = num_visible_rows - 1;  // Reserve 1 for center row
                            int rows_above_center = rows_available / 2;  // Equal split of remaining rows
                            int rows_below_center = rows_available - rows_above_center;  // Rest go below
                            // Keep symmetric: same number above and below (6 and 6 typically)
                            // No extra rows added - keeps it balanced
                            num_visible_rows = rows_above_center + 1 + rows_below_center;  // above + center + below
                            int center_row_idx = rows_above_center;  // Center is after all rows above
                            // Example: 13 visible rows -> 12 available, 6 above + 1 center + 6 below = 13 total
                            
                            // Vertically center all rows in the available space
                            // First, calculate how many rows will actually fit (check if last row would overflow)
                            int max_row_y = header_y + (num_visible_rows - 1) * line_height + line_height;
                            int actual_rows_count = num_visible_rows;
                            if (max_row_y > logical_height - MARGIN_BOTTOM) {
                                // Last row would overflow, so we'll skip it - adjust count for centering
                                actual_rows_count = num_visible_rows - 1;
                            }
                            
                            // Calculate total height needed for rows that will actually be drawn
                            int total_rows_height = actual_rows_count * line_height;
                            // Calculate vertical offset to center the rows in scrollable_height
                            int vertical_offset = (scrollable_height - total_rows_height) / 2;
                            // Calculate starting Y position (header_y + offset to center rows)
                            int rows_start_y = header_y + vertical_offset;
                            
                            // Calculate actual center Y position based on center_row_idx
                            // This matches the fixed grid - center_y for first render will be recalculated if needed
                            int actual_center_y = rows_start_y + (center_row_idx * line_height);
                            center_y = actual_center_y;  // Update center_y to match fixed grid
                            
                            // Clear content area before drawing
                            ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, header_y, 
                                         FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT, scrollable_height, RGB565_BLACK);
                            
                            // Draw all visible rows at fixed positions (vertically centered)
                            for (int row_idx = 0; row_idx < num_visible_rows; row_idx++) {
                                int y = rows_start_y + (row_idx * line_height);  // Fixed position for this row, vertically centered
                                
                                // Skip rows that would overflow below the visible area
                                // This prevents the extra pending row from showing remnants at the bottom
                                if (y + line_height > logical_height - MARGIN_BOTTOM) {
                                    continue;  // Skip this row - it's partially or fully off-screen
                                }
                                
                                // Determine which data to draw for this row index
                                app_tracker_tick_t *tick = NULL;
                                bool is_current_row = false;
                                
                                if (row_idx < center_row_idx) {
                                    // Rows above center: history rows (most recent closest to center)
                                    int history_offset = center_row_idx - row_idx - 1;  // 0 = most recent, 1 = older, etc.
                                    if (history_offset < state->tracker.tick_history_count) {
                                        int history_idx = state->tracker.tick_history_count - 1 - history_offset;  // Most recent is last in history
                                        if (history_idx >= 0) {
                                            tick = &state->tracker.tick_history[history_idx];
                                        }
                                    }
                                } else if (row_idx == center_row_idx) {
                                    // Center row: ALWAYS use frame_info (current playing row)
                                    is_current_row = true;
                                    tick = NULL;  // Always use frame_info for current row
                                } else {
                                    // Rows below center: future rows from pattern
                                    tick = NULL;  // Will fetch from pattern
                                }
                                
                                // Now draw this row at fixed position y
                                // Get row data - either from tick, frame_info (for current), or pattern (for future)
                                struct xmp_channel_info row_channels[MOD_MAX_CHANNELS];
                                bool has_data = false;
                                int future_row_num = -1;
                                int future_pattern = -1;  // Pattern to use for future rows (may differ from current pattern)
                                
                                if (tick != NULL) {
                                    // Use tick data (history rows)
                                    memcpy(row_channels, tick->channels, sizeof(tick->channels));
                                    has_data = true;
                                } else if (is_current_row) {
                                    // Current row - ALWAYS use frame_info (what's actually playing now)
                                    memcpy(row_channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                                    has_data = true;
                                } else if (row_idx > center_row_idx) {
                                    // Future row - fetch from current pattern only (no peek-ahead to next pattern)
                                    int future_offset = row_idx - center_row_idx - 1;  // 0 = next row, 1 = row after, etc.
                                    int target_row = frame_info.row + future_offset + 1;  // +1 because next row is after current
                                    
                                    // Try to fetch from current pattern at target row
                                    uint8_t test_note = 0;
                                    esp_err_t test_ret = mod_player_get_pattern_row_channel(frame_info.pattern, target_row, 0, &test_note, NULL, NULL, NULL);
                                    
                                    if (test_ret == ESP_OK) {
                                        // Pattern access works - will fetch all channels
                                        future_pattern = frame_info.pattern;
                                        future_row_num = target_row;
                                        has_data = true;
                                    } else {
                                        // Pattern access failed (out of bounds) - show empty row
                                        has_data = false;
                                        future_row_num = -1;
                                        future_pattern = -1;
                                    }
                                }
                                
                                // Calculate text fade factor based on distance from center row
                                // Text fades from full brightness at center to near-black at edges
                                float text_fade = 1.0f;  // 1.0 = full brightness, 0.0 = black
                                if (!is_current_row) {
                                    int distance_from_center;
                                    int max_distance;
                                    if (row_idx < center_row_idx) {
                                        distance_from_center = center_row_idx - row_idx;
                                        max_distance = rows_above_center;
                                    } else {
                                        distance_from_center = row_idx - center_row_idx;
                                        max_distance = rows_below_center;
                                    }
                                    float darkness = (max_distance > 0) ? (float)distance_from_center / (float)max_distance : 0.0f;
                                    if (darkness > 1.0f) darkness = 1.0f;
                                    // Linear falloff, capped at 70% darkness to keep text legible
                                    darkness = darkness * 0.7f;
                                    text_fade = 1.0f - darkness;
                                }

                                // Draw row if we have data
                                if (has_data) {
                                    // Draw highlight background for current row only
                                    if (is_current_row) {
                                        int highlight_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, y, highlight_width, line_height, RGB565_DARK_BLUE);
                                    }

                                    // Calculate font scale
                                    int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                    if (font_scale < 1) font_scale = 1;
                                    if (font_scale > 3) font_scale = 3;
                                    
                                    // Calculate total width of visible channels
                                    int total_width = 0;
                                    int channel_widths[4];  // Max 4 channels per page
                                    const char *separator = " ";
                                    int separator_width = strlen(separator) * FONT_WIDTH * font_scale;
                                    
                                    for (int i = 0; i < visible_channels; i++) {
                                        int ch = start_channel + i;
                                        if (ch >= num_channels) break;
                                        
                                        uint8_t note = 0, ins = 0, fxt = 0, fxp = 0;
                                        
                                        if (future_row_num >= 0) {
                                            // Future row - fetch from pattern (may be from next pattern if we're at end of current)
                                            int pattern_to_use = (future_pattern >= 0) ? future_pattern : frame_info.pattern;
                                            esp_err_t pat_ret = mod_player_get_pattern_row_channel(pattern_to_use, future_row_num, ch, &note, &ins, &fxt, &fxp);
                                            if (pat_ret != ESP_OK) {
                                                // Pattern access failed - skip this row
                                                has_data = false;
                                                break;
                                            }
                                        } else {
                                            // Use row_channels data
                                            note = row_channels[ch].event.note;
                                            ins = row_channels[ch].event.ins;
                                            fxt = row_channels[ch].event.fxt;
                                            fxp = row_channels[ch].event.fxp;
                                        }
                                        
                                        char ch_str[64];
                                        format_channel_string(ch_str, sizeof(ch_str), note, ins, fxt, fxp, use_compact_format, note_names);
                                        channel_widths[i] = strlen(ch_str) * FONT_WIDTH * font_scale;
                                        total_width += channel_widths[i];
                                        if (i < visible_channels - 1) {
                                            total_width += separator_width;
                                        }
                                    }
                                    
                                    // Only draw if we successfully got data for all channels
                                    if (has_data) {
                                        // Calculate starting X position to center the channels
                                        int content_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                                        int start_x = MARGIN_LEFT + (content_width - total_width) / 2;

                                        // Cache channel positions for VU meter alignment (do once per frame on current row)
                                        if (is_current_row) {
                                            int cache_x = start_x;
                                            for (int i = 0; i < visible_channels && i < 4; i++) {
                                                state->vu_state.channel_x[i] = cache_x;
                                                state->vu_state.channel_width[i] = channel_widths[i];
                                                cache_x += channel_widths[i];
                                                if (i < visible_channels - 1) {
                                                    cache_x += separator_width;
                                                }
                                            }
                                        }

                                        // Render channels centered
                                        int x = start_x;
                                        for (int i = 0; i < visible_channels; i++) {
                                            int ch = start_channel + i;
                                            if (ch >= num_channels) break;
                                            
                                            uint8_t note = 0, ins = 0, fxt = 0, fxp = 0;
                                            if (future_row_num >= 0) {
                                                // Future row - use future_pattern if we're at end of current pattern
                                                int pattern_to_use = (future_pattern >= 0) ? future_pattern : frame_info.pattern;
                                                mod_player_get_pattern_row_channel(pattern_to_use, future_row_num, ch, &note, &ins, &fxt, &fxp);
                                            } else {
                                                note = row_channels[ch].event.note;
                                                ins = row_channels[ch].event.ins;
                                                fxt = row_channels[ch].event.fxt;
                                                fxp = row_channels[ch].event.fxp;
                                            }
                                            
                                            char ch_str[64];
                                            format_channel_string(ch_str, sizeof(ch_str), note, ins, fxt, fxp, use_compact_format, note_names);
                                            
                                            bool channel_muted = false;
                                            mod_player_get_channel_mute(ch, &channel_muted);
                                            uint16_t ch_color = channel_muted ? THEME_TEXT_MUTED : theme_channel_color(ch);

                                            // Apply text fade based on distance from center row
                                            if (text_fade < 1.0f) {
                                                uint8_t fade_alpha = (uint8_t)(text_fade * 255.0f);
                                                ch_color = theme_blend_colors(ch_color, THEME_BG_PRIMARY, fade_alpha);
                                            }

                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                            x += channel_widths[i];
                                            
                                            // Draw separator and vertical line between channels (not after last)
                                            if (i < visible_channels - 1) {
                                                uint16_t sep_color = RGB565_WHITE;
                                                if (text_fade < 1.0f) {
                                                    uint8_t fade_alpha = (uint8_t)(text_fade * 255.0f);
                                                    sep_color = theme_blend_colors(sep_color, THEME_BG_PRIMARY, fade_alpha);
                                                }
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y, sep_color, font_scale, separator);
                                                int line_x = x + separator_width / 2;  // Middle of the space
                                                int line_top = header_y;  // Start from header bottom
                                                int line_bottom = logical_height - MARGIN_BOTTOM;  // End at content bottom
                                                ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT, line_x, line_top, 1, line_bottom - line_top, RGB565_GRAY);
                                                x += separator_width;
                                            }
                                        }
                                    }
                                } else if (row_idx > center_row_idx) {
                                    // No data for future row (beyond pattern bounds) - draw empty row
                                    // Calculate font scale
                                    int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                    if (font_scale < 1) font_scale = 1;
                                    if (font_scale > 3) font_scale = 3;

                                    // Create empty row string "--- -- ----" for each channel
                                    char empty_str[64];
                                    format_channel_string(empty_str, sizeof(empty_str), 0, 0, 0, 0, use_compact_format, note_names);
                                    int empty_width = strlen(empty_str) * FONT_WIDTH * font_scale;

                                    const char *separator = " ";
                                    int separator_width = strlen(separator) * FONT_WIDTH * font_scale;

                                    int total_width = visible_channels * empty_width + (visible_channels - 1) * separator_width;
                                    int content_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                                    int start_x = MARGIN_LEFT + (content_width - total_width) / 2;

                                    int x = start_x;
                                    for (int i = 0; i < visible_channels; i++) {
                                        int ch = start_channel + i;
                                        uint16_t ch_color = theme_channel_color(ch);

                                        // Apply text fade based on distance from center row
                                        if (text_fade < 1.0f) {
                                            uint8_t fade_alpha = (uint8_t)(text_fade * 255.0f);
                                            ch_color = theme_blend_colors(ch_color, THEME_BG_PRIMARY, fade_alpha);
                                        }

                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, empty_str);
                                        x += empty_width;

                                        if (i < visible_channels - 1) {
                                            uint16_t sep_color = RGB565_WHITE;
                                            if (text_fade < 1.0f) {
                                                uint8_t fade_alpha = (uint8_t)(text_fade * 255.0f);
                                                sep_color = theme_blend_colors(sep_color, THEME_BG_PRIMARY, fade_alpha);
                                            }
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y, sep_color, font_scale, separator);
                                            x += separator_width;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Draw view-dependent overlays
                    if (state->current_view == APP_VIEW_TRACKER) {
                        // Layout: VU meters above hint bar at bottom
                        const int hint_bar_height = 20;  // Height for hint bar
                        int vu_height = state->vu_state.cached_line_height;
                        int hint_bar_y = FB_HEIGHT - MARGIN_BOTTOM - hint_bar_height;
                        int vu_footer_y = hint_bar_y - vu_height;

                        // Draw background for VU area
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      0, vu_footer_y, FB_WIDTH, vu_height, THEME_BG_SECONDARY);

                        // Draw VU meters aligned with channel columns (using cached positions)
                        int visible_ch = state->vu_state.visible_channels;
                        if (visible_ch < 1) visible_ch = 4;
                        for (int i = 0; i < visible_ch && i < 4; i++) {
                            int ch = state->vu_state.start_channel + i;
                            if (ch >= num_channels || ch >= MOD_MAX_CHANNELS) break;

                            // Use cached column positions for alignment
                            int vu_x = state->vu_state.channel_x[i];
                            int vu_width = state->vu_state.channel_width[i];
                            if (vu_width < 10) vu_width = 50;  // Fallback if not cached yet

                            ui_draw_vu_meter(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                             vu_x, vu_footer_y + 2, vu_width, vu_height - 4,
                                             state->vu_state.levels[ch], state->vu_state.peaks[ch],
                                             UI_VU_HORIZONTAL);

                            // Draw channel number label centered horizontally and vertically
                            char ch_label[16];
                            snprintf(ch_label, sizeof(ch_label), "CH%d", ch + 1);
                            int label_width = strlen(ch_label) * FONT_WIDTH;
                            int label_x = vu_x + (vu_width - label_width) / 2;
                            int label_y = vu_footer_y + (vu_height - FONT_HEIGHT) / 2;
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    label_x, label_y,
                                                    THEME_TEXT_MUTED, 1, ch_label);
                        }

                        // Draw hint bar at bottom
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      0, hint_bar_y, FB_WIDTH, hint_bar_height, THEME_BG_PRIMARY);

                        // Draw playback hints - get icon dimensions for proper positioning
                        int icon_width = 0, icon_height = 0;
                        int gap = 20;

                        // Get actual icon size if available
                        if (fkey_icon_available(3)) {
                            fkey_icon_get_size(3, &icon_width, &icon_height);
                        }
                        if (icon_width == 0) icon_width = 16;
                        if (icon_height == 0) icon_height = 16;

                        // Calculate total width for centering
                        // F3 View + F4 Channel page + F6 Exit + arrows Vol + space Pause
                        int total_width = 0;
                        total_width += icon_width + 4 + FONT_WIDTH * 4 + gap;   // F3 View
                        total_width += icon_width + 4 + FONT_WIDTH * 12 + gap;  // F4 Channel page
                        total_width += icon_width + 4 + FONT_WIDTH * 4 + gap;   // F6 Exit
                        total_width += FONT_WIDTH * 6 + gap;                     // arrows Vol
                        total_width += FONT_WIDTH * 8;                           // space Pause/Resume

                        int hint_x = (FB_WIDTH - total_width) / 2;

                        // Calculate vertical centering
                        // Font has more bottom padding than top, so add 1px offset to visually center text with icons
                        int icon_y = hint_bar_y + (hint_bar_height - icon_height) / 2;
                        int text_y = hint_bar_y + (hint_bar_height - FONT_HEIGHT) / 2 + 1;

                        // F3 - View
                        if (fkey_icon_available(3)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, icon_y, 3, 1);
                            hint_x += icon_width + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "F3");
                            hint_x += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "View");
                        hint_x += FONT_WIDTH * 4 + gap;

                        // F4 - Channel page
                        if (fkey_icon_available(4)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, icon_y, 4, 1);
                            hint_x += icon_width + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "F4");
                            hint_x += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "Channel page");
                        hint_x += FONT_WIDTH * 12 + gap;

                        // F6 - Exit
                        if (fkey_icon_available(6)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, icon_y, 6, 1);
                            hint_x += icon_width + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "F6");
                            hint_x += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "Exit");
                        hint_x += FONT_WIDTH * 4 + gap;

                        // Up/Down arrows - Vol (chars 128, 129 = 0x80, 0x81)
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, "\x80\x81 Vol");
                        hint_x += FONT_WIDTH * 6 + gap;

                        // Space bar - Pause/Resume (char 133 = 0x85)
                        const char *pause_text = mod_player_is_paused() ? "\x85 Resume" : "\x85 Pause";
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, hint_x, text_y, THEME_TEXT_MUTED, 1, pause_text);
                    } else if (state->current_view == APP_VIEW_SPECTRUM) {
                        // Full-screen spectrum analyzer view
                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_SPECTRUM_BG);

                        // Draw header with song info
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, MARGIN_TOP,
                                                THEME_TEXT_PRIMARY, 2, "SPECTRUM ANALYZER");

                        // Draw spectrum bars
                        uint8_t bands[SPECTRUM_NUM_BANDS] = {0};
                        uint8_t peaks[SPECTRUM_NUM_BANDS] = {0};
                        if (state->spectrum) {
                            spectrum_get_bands_and_peaks(state->spectrum, bands, peaks, SPECTRUM_NUM_BANDS);
                            spectrum_update_decay(state->spectrum);
                        } else {
                            // No spectrum analyzer - show simulated bars from channel volumes
                            for (int i = 0; i < SPECTRUM_NUM_BANDS && i < num_channels; i++) {
                                bands[i] = (uint8_t)(state->vu_state.levels[i] * 255.0f);
                                peaks[i] = (uint8_t)(state->vu_state.peaks[i] * 255.0f);
                            }
                        }

                        const int hint_bar_height_spec = 20;
                        int spec_y = MARGIN_TOP + 50;
                        int spec_height = FB_HEIGHT - spec_y - MARGIN_BOTTOM - hint_bar_height_spec;
                        ui_draw_spectrum_bars(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                              MARGIN_LEFT, spec_y, CONTENT_WIDTH, spec_height,
                                              bands, SPECTRUM_NUM_BANDS, peaks, 4);

                        // Footer hint bar (same style as tracker view)
                        int spec_hint_y = FB_HEIGHT - MARGIN_BOTTOM - hint_bar_height_spec;
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      0, spec_hint_y, FB_WIDTH, hint_bar_height_spec, THEME_BG_PRIMARY);

                        int spec_icon_w = 16, spec_icon_h = 16;
                        if (fkey_icon_available(3)) fkey_icon_get_size(3, &spec_icon_w, &spec_icon_h);
                        int spec_gap = 20;
                        int spec_total = (spec_icon_w + 4 + FONT_WIDTH * 4 + spec_gap) * 2 +  // F3 View, F6 Exit
                                         FONT_WIDTH * 6 + spec_gap + FONT_WIDTH * 8;          // arrows Vol, space Pause
                        int spec_hx = (FB_WIDTH - spec_total) / 2;
                        int spec_icon_y = spec_hint_y + (hint_bar_height_spec - spec_icon_h) / 2;
                        int spec_text_y = spec_hint_y + (hint_bar_height_spec - FONT_HEIGHT) / 2 + 1;

                        // F3 View
                        if (fkey_icon_available(3)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_icon_y, 3, 1);
                            spec_hx += spec_icon_w + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_text_y, THEME_TEXT_MUTED, 1, "F3");
                            spec_hx += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_text_y, THEME_TEXT_MUTED, 1, "View");
                        spec_hx += FONT_WIDTH * 4 + spec_gap;

                        // F6 Exit
                        if (fkey_icon_available(6)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_icon_y, 6, 1);
                            spec_hx += spec_icon_w + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_text_y, THEME_TEXT_MUTED, 1, "F6");
                            spec_hx += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_text_y, THEME_TEXT_MUTED, 1, "Exit");
                        spec_hx += FONT_WIDTH * 4 + spec_gap;

                        // arrows Vol
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_text_y, THEME_TEXT_MUTED, 1, "\x80\x81 Vol");
                        spec_hx += FONT_WIDTH * 6 + spec_gap;

                        // space Pause
                        const char *spec_pause = mod_player_is_paused() ? "\x85 Resume" : "\x85 Pause";
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, spec_hx, spec_text_y, THEME_TEXT_MUTED, 1, spec_pause);
                    } else if (state->current_view == APP_VIEW_INFO) {
                        // Module info view
                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);

                        // Header
                        ui_draw_vgradient(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                          0, MARGIN_TOP, FB_WIDTH, THEME_HEADER_HEIGHT,
                                          THEME_BG_HEADER, THEME_BG_PRIMARY);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, MARGIN_TOP + 4,
                                                THEME_TEXT_PRIMARY, 2, "MODULE INFO");

                        int info_y = MARGIN_TOP + THEME_HEADER_HEIGHT + 20;
                        int line_spacing = 28;

                        // Module name
                        char info_line[128];
                        const char *mod_name = state->tracker.mod_info.mod ? state->tracker.mod_info.mod->name : "Unknown";
                        snprintf(info_line, sizeof(info_line), "Name: %s", mod_name);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_PRIMARY, 2, info_line);
                        info_y += line_spacing;

                        // Channels
                        snprintf(info_line, sizeof(info_line), "Channels: %d", num_channels);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_SECONDARY, 2, info_line);
                        info_y += line_spacing;

                        // Patterns
                        int pat_count = state->tracker.mod_info.mod ? state->tracker.mod_info.mod->pat : 0;
                        snprintf(info_line, sizeof(info_line), "Patterns: %d", pat_count);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_SECONDARY, 2, info_line);
                        info_y += line_spacing;

                        // Current position
                        int song_len = state->tracker.mod_info.mod ? state->tracker.mod_info.mod->len : 0;
                        snprintf(info_line, sizeof(info_line), "Position: %d / %d", frame_info.pos, song_len);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_SECONDARY, 2, info_line);
                        info_y += line_spacing;

                        // Speed/BPM
                        snprintf(info_line, sizeof(info_line), "Speed: %d | BPM: %d", frame_info.speed, frame_info.bpm);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_SECONDARY, 2, info_line);
                        info_y += line_spacing * 2;

                        // Channel VU meters in a grid
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_PRIMARY, 2, "Channel Levels:");
                        info_y += line_spacing;

                        int vu_per_row = 8;
                        int vu_width = (CONTENT_WIDTH - (vu_per_row - 1) * 8) / vu_per_row;
                        for (int ch = 0; ch < num_channels && ch < MOD_MAX_CHANNELS; ch++) {
                            int row = ch / vu_per_row;
                            int col = ch % vu_per_row;
                            int vu_x = MARGIN_LEFT + col * (vu_width + 8);
                            int vu_y = info_y + row * 24;

                            // Channel number label
                            char ch_label[4];
                            snprintf(ch_label, sizeof(ch_label), "%d", ch + 1);
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    vu_x, vu_y, THEME_TEXT_MUTED, 1, ch_label);

                            // VU bar
                            ui_draw_vu_bar(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                           vu_x + 16, vu_y + 2, vu_width - 20, 12,
                                           state->vu_state.levels[ch], THEME_VU_BG);
                        }

                        // Footer hint bar (same style as tracker view)
                        int info_hint_h = 20;
                        int info_hint_y = FB_HEIGHT - MARGIN_BOTTOM - info_hint_h;
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      0, info_hint_y, FB_WIDTH, info_hint_h, THEME_BG_PRIMARY);

                        int info_icon_w = 16, info_icon_h = 16;
                        if (fkey_icon_available(3)) fkey_icon_get_size(3, &info_icon_w, &info_icon_h);
                        int info_gap = 20;
                        int info_total = (info_icon_w + 4 + FONT_WIDTH * 4 + info_gap) * 2 +
                                         FONT_WIDTH * 6 + info_gap + FONT_WIDTH * 8;
                        int info_hx = (FB_WIDTH - info_total) / 2;
                        int info_icon_y = info_hint_y + (info_hint_h - info_icon_h) / 2;
                        int info_text_y = info_hint_y + (info_hint_h - FONT_HEIGHT) / 2 + 1;

                        // F3 View
                        if (fkey_icon_available(3)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_icon_y, 3, 1);
                            info_hx += info_icon_w + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_text_y, THEME_TEXT_MUTED, 1, "F3");
                            info_hx += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_text_y, THEME_TEXT_MUTED, 1, "View");
                        info_hx += FONT_WIDTH * 4 + info_gap;

                        // F6 Exit
                        if (fkey_icon_available(6)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_icon_y, 6, 1);
                            info_hx += info_icon_w + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_text_y, THEME_TEXT_MUTED, 1, "F6");
                            info_hx += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_text_y, THEME_TEXT_MUTED, 1, "Exit");
                        info_hx += FONT_WIDTH * 4 + info_gap;

                        // arrows Vol
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_text_y, THEME_TEXT_MUTED, 1, "\x80\x81 Vol");
                        info_hx += FONT_WIDTH * 6 + info_gap;

                        // space Pause
                        const char *info_pause = mod_player_is_paused() ? "\x85 Resume" : "\x85 Pause";
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, info_hx, info_text_y, THEME_TEXT_MUTED, 1, info_pause);
                    }
                    else if (state->current_view == APP_VIEW_VGM) {
                        // VGM playback view
                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);

                        // Get VGM tags and playback info
                        vgm_tags_t vgm_tags;
                        vgm_playback_info_t vgm_info;
                        vgm_player_get_tags(&vgm_tags);
                        vgm_player_get_info(&vgm_info);

                        // Header (match MOD header style)
                        const int header_height = FONT_HEIGHT + 4;  // shared header height
                        ui_draw_vgradient(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                          0, MARGIN_TOP, FB_WIDTH, header_height,
                                          THEME_BG_HEADER, THEME_BG_PRIMARY);

                        // Title (or filename) on the left
                        const char *vgm_title = NULL;
                        if (vgm_tags.title[0]) {
                            vgm_title = vgm_tags.title;
                        } else {
                            const char *fname = strrchr(state->mod_file.path, '/');
                            vgm_title = fname ? fname + 1 : state->mod_file.path;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, MARGIN_TOP + 2,
                                                THEME_TEXT_PRIMARY, 1, vgm_title);

                        // Volume on the right (same as MOD header)
                        float header_vol = 0.0f;
                        if (audio_get_volume(&header_vol) == ESP_OK) {
                            char vol_text[16];
                            int vol_percent = (int)((header_vol - 0.20f) / 0.80f * 100.0f + 0.5f);
                            if (vol_percent < 0) vol_percent = 0;
                            if (vol_percent > 100) vol_percent = 100;
                            snprintf(vol_text, sizeof(vol_text), "Vol: %d%%", vol_percent);
                            int vol_width = strlen(vol_text) * FONT_WIDTH;  // font_scale = 1
                            int vol_x = FB_WIDTH - MARGIN_RIGHT - vol_width;
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    vol_x, MARGIN_TOP + 2,
                                                    THEME_TEXT_PRIMARY, 1, vol_text);
                        }

                        int vgm_y = MARGIN_TOP + header_height + 16;
                        int line_spacing = FONT_HEIGHT + 8;
                        const int meta_scale = 1;
                        char vgm_line[128];

                        // Game
                        if (vgm_tags.game[0]) {
                            snprintf(vgm_line, sizeof(vgm_line), "Game: %.50s", vgm_tags.game);
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    MARGIN_LEFT, vgm_y, THEME_TEXT_SECONDARY, meta_scale, vgm_line);
                            vgm_y += line_spacing;
                        }

                        // System
                        if (vgm_tags.system[0]) {
                            snprintf(vgm_line, sizeof(vgm_line), "System: %.50s", vgm_tags.system);
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    MARGIN_LEFT, vgm_y, THEME_TEXT_SECONDARY, meta_scale, vgm_line);
                            vgm_y += line_spacing;
                        }

                        // Author
                        if (vgm_tags.author[0]) {
                            snprintf(vgm_line, sizeof(vgm_line), "Author: %.50s", vgm_tags.author);
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    MARGIN_LEFT, vgm_y, THEME_TEXT_SECONDARY, meta_scale, vgm_line);
                            vgm_y += line_spacing;
                        }

                        // Chip info
                        vgm_y += 8;
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, vgm_y, THEME_TEXT_MUTED, meta_scale, "Chips:");

                        // Display active chips on same line
                        int chip_x = MARGIN_LEFT + (FONT_WIDTH * meta_scale * 6);
                        for (int i = 0; i < vgm_info.num_chips && i < 4; i++) {
                            vgm_chip_info_t chip;
                            if (vgm_player_get_chip_info(i, &chip) == ESP_OK && chip.name) {
                                snprintf(vgm_line, sizeof(vgm_line), "[%s]", chip.name);
                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                        chip_x, vgm_y, THEME_VU_MID, meta_scale, vgm_line);
                                chip_x += strlen(vgm_line) * FONT_WIDTH * meta_scale + 8;
                            }
                        }
                        vgm_y += line_spacing + 16;

                        // Progress bar
                        int progress_bar_w = CONTENT_WIDTH - 120;
                        int progress_bar_h = 16;
                        int progress_bar_x = MARGIN_LEFT;
                        int progress_bar_y = vgm_y;

                        // Background
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      progress_bar_x, progress_bar_y, progress_bar_w, progress_bar_h,
                                      THEME_VU_BG);

                        // Progress fill
                        double progress = 0.0;
                        if (vgm_info.total_time_sec > 0) {
                            progress = vgm_info.current_time_sec / vgm_info.total_time_sec;
                            if (progress > 1.0) progress = 1.0;
                        }
                        int fill_w = (int)(progress_bar_w * progress);
                        if (fill_w > 0) {
                            ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                          progress_bar_x, progress_bar_y, fill_w, progress_bar_h,
                                          THEME_VU_MID);
                        }

                        // Time display
                        int cur_min = (int)(vgm_info.current_time_sec / 60);
                        int cur_sec = (int)(vgm_info.current_time_sec) % 60;
                        int tot_min = (int)(vgm_info.total_time_sec / 60);
                        int tot_sec = (int)(vgm_info.total_time_sec) % 60;
                        snprintf(vgm_line, sizeof(vgm_line), "%d:%02d / %d:%02d", cur_min, cur_sec, tot_min, tot_sec);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                progress_bar_x + progress_bar_w + 8, progress_bar_y,
                                                THEME_TEXT_SECONDARY, 1, vgm_line);

                        // Oscilloscope waveform visualization
                        vgm_y = progress_bar_y + progress_bar_h + 24;
                        int scope_w = CONTENT_WIDTH;
                        int scope_h = 120;  // Height for both channels
                        int scope_x = MARGIN_LEFT;
                        int scope_y = vgm_y;
                        int channel_h = (scope_h - 8) / 2;  // Height per channel with gap

                        // Scope background
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      scope_x, scope_y, scope_w, scope_h, THEME_VU_BG);

                        // Get waveform data
                        static int16_t wave_left[512];
                        static int16_t wave_right[512];
                        size_t wave_samples = 0;
                        if (vgm_player_get_waveform(wave_left, wave_right, 512, &wave_samples) == ESP_OK && wave_samples > 0) {
                            // Draw left channel (green) - top half
                            int left_center_y = scope_y + channel_h / 2;
                            int prev_left_y = left_center_y;
                            for (size_t i = 0; i < wave_samples && i < (size_t)scope_w; i++) {
                                int x = scope_x + (int)(i * scope_w / wave_samples);
                                // Map -32768..32767 to -channel_h/2..channel_h/2
                                int y_offset = (wave_left[i] * (channel_h / 2)) / 32768;
                                int y = left_center_y - y_offset;
                                if (y < scope_y) y = scope_y;
                                if (y >= scope_y + channel_h) y = scope_y + channel_h - 1;
                                // Draw vertical line from prev_y to y for smoothness
                                if (i > 0) {
                                    int y1 = (prev_left_y < y) ? prev_left_y : y;
                                    int y2 = (prev_left_y > y) ? prev_left_y : y;
                                    ui_draw_vline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y1, y2 - y1 + 1, THEME_VU_LOW);
                                }
                                prev_left_y = y;
                            }

                            // Draw right channel (cyan) - bottom half
                            int right_center_y = scope_y + channel_h + 8 + channel_h / 2;
                            int prev_right_y = right_center_y;
                            for (size_t i = 0; i < wave_samples && i < (size_t)scope_w; i++) {
                                int x = scope_x + (int)(i * scope_w / wave_samples);
                                int y_offset = (wave_right[i] * (channel_h / 2)) / 32768;
                                int y = right_center_y - y_offset;
                                if (y < scope_y + channel_h + 8) y = scope_y + channel_h + 8;
                                if (y >= scope_y + scope_h) y = scope_y + scope_h - 1;
                                if (i > 0) {
                                    int y1 = (prev_right_y < y) ? prev_right_y : y;
                                    int y2 = (prev_right_y > y) ? prev_right_y : y;
                                    ui_draw_vline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y1, y2 - y1 + 1, THEME_ACCENT_6);
                                }
                                prev_right_y = y;
                            }

                            // Draw center lines (dim)
                            ui_draw_hline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, scope_x, left_center_y, scope_w, THEME_TEXT_MUTED);
                            ui_draw_hline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, scope_x, right_center_y, scope_w, THEME_TEXT_MUTED);
                        } else {
                            // No waveform data - draw center lines only
                            ui_draw_hline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, scope_x, scope_y + channel_h / 2, scope_w, THEME_TEXT_MUTED);
                            ui_draw_hline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, scope_x, scope_y + channel_h + 8 + channel_h / 2, scope_w, THEME_TEXT_MUTED);
                        }

                        // Channel labels
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                scope_x + 4, scope_y + 2, THEME_VU_LOW, 1, "L");
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                scope_x + 4, scope_y + channel_h + 10, THEME_ACCENT_6, 1, "R");

                        vgm_y = scope_y + scope_h + 8;

                        // Loop indicator
                        if (vgm_info.has_loop) {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                    MARGIN_LEFT, vgm_y, THEME_TEXT_MUTED, 1, "(Looping track)");
                            vgm_y += line_spacing;
                        }

                        // Footer hint bar
                        const int vgm_hint_h = 20;
                        int vgm_hint_y = FB_HEIGHT - MARGIN_BOTTOM - vgm_hint_h;
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      0, vgm_hint_y, FB_WIDTH, vgm_hint_h, THEME_BG_PRIMARY);

                        int vgm_icon_w = 16, vgm_icon_h = 16;
                        if (fkey_icon_available(6)) fkey_icon_get_size(6, &vgm_icon_w, &vgm_icon_h);
                        int vgm_gap = 20;
                        int vgm_total = (vgm_icon_w + 4 + FONT_WIDTH * 4 + vgm_gap) +  // F6 Exit
                                        (FONT_WIDTH * 6 + vgm_gap) +                   // arrows Vol
                                        (FONT_WIDTH * 8);                              // space Pause
                        int vgm_hx = (FB_WIDTH - vgm_total) / 2;
                        int vgm_icon_y_pos = vgm_hint_y + (vgm_hint_h - vgm_icon_h) / 2;
                        int vgm_text_y = vgm_hint_y + (vgm_hint_h - FONT_HEIGHT) / 2 + 1;

                        // F6 Exit
                        if (fkey_icon_available(6)) {
                            fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_icon_y_pos, 6, 1);
                            vgm_hx += vgm_icon_w + 4;
                        } else {
                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, "F6");
                            vgm_hx += FONT_WIDTH * 2 + 4;
                        }
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, "Exit");
                        vgm_hx += FONT_WIDTH * 4 + vgm_gap;

                        // arrows Vol
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, "\x80\x81 Vol");
                        vgm_hx += FONT_WIDTH * 6 + vgm_gap;

                        // space Pause
                        const char *vgm_pause = vgm_player_is_paused() ? "\x85 Resume" : "\x85 Pause";
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, vgm_pause);
                    }

                    PROFILING_END(render, render);
                    // Note: state->tracker.last_row is now updated when we finish scrolling, not immediately
                    // This allows smooth scrolling to complete before marking the row as processed
                    did_render = true;  // Tracker UI always renders (smooth scrolling)
                }
            }
            else if (vgm_player_is_playing() && state->current_view == APP_VIEW_VGM) {
                // VGM is playing but MOD is not - render VGM view directly
                PROFILING_START(render);

                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);

                // Get VGM tags and playback info
                vgm_tags_t vgm_tags;
                vgm_playback_info_t vgm_info;
                vgm_player_get_tags(&vgm_tags);
                vgm_player_get_info(&vgm_info);

                // Header (match MOD header style)
                const int header_height = FONT_HEIGHT + 4;  // shared header height
                ui_draw_vgradient(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                  0, MARGIN_TOP, FB_WIDTH, header_height,
                                  THEME_BG_HEADER, THEME_BG_PRIMARY);

                // Title (or filename) on the left
                const char *vgm_title = NULL;
                if (vgm_tags.title[0]) {
                    vgm_title = vgm_tags.title;
                } else {
                    const char *fname = strrchr(state->mod_file.path, '/');
                    vgm_title = fname ? fname + 1 : state->mod_file.path;
                }
                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                        MARGIN_LEFT, MARGIN_TOP + 2,
                                        THEME_TEXT_PRIMARY, 1, vgm_title);

                // Volume on the right
                float header_vol = 0.0f;
                if (audio_get_volume(&header_vol) == ESP_OK) {
                    char vol_text[16];
                    int vol_percent = (int)((header_vol - 0.20f) / 0.80f * 100.0f + 0.5f);
                    if (vol_percent < 0) vol_percent = 0;
                    if (vol_percent > 100) vol_percent = 100;
                    snprintf(vol_text, sizeof(vol_text), "Vol: %d%%", vol_percent);
                    int vol_width = strlen(vol_text) * FONT_WIDTH;  // font_scale = 1
                    int vol_x = FB_WIDTH - MARGIN_RIGHT - vol_width;
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                            vol_x, MARGIN_TOP + 2,
                                            THEME_TEXT_PRIMARY, 1, vol_text);
                }

                int vgm_y = MARGIN_TOP + header_height + 16;
                int line_spacing = FONT_HEIGHT + 8;
                const int meta_scale = 1;
                char vgm_line[128];

                // Game
                if (vgm_tags.game[0]) {
                    snprintf(vgm_line, sizeof(vgm_line), "Game: %.50s", vgm_tags.game);
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                            MARGIN_LEFT, vgm_y, THEME_TEXT_SECONDARY, meta_scale, vgm_line);
                    vgm_y += line_spacing;
                }

                // System
                if (vgm_tags.system[0]) {
                    snprintf(vgm_line, sizeof(vgm_line), "System: %.50s", vgm_tags.system);
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                            MARGIN_LEFT, vgm_y, THEME_TEXT_SECONDARY, meta_scale, vgm_line);
                    vgm_y += line_spacing;
                }

                // Author
                if (vgm_tags.author[0]) {
                    snprintf(vgm_line, sizeof(vgm_line), "Author: %.50s", vgm_tags.author);
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                            MARGIN_LEFT, vgm_y, THEME_TEXT_SECONDARY, meta_scale, vgm_line);
                    vgm_y += line_spacing;
                }

                // Chip info - display on same line as label
                vgm_y += 8;
                int chip_x = MARGIN_LEFT;
                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                        chip_x, vgm_y, THEME_TEXT_MUTED, meta_scale, "Chips:");
                chip_x += 6 * FONT_WIDTH * meta_scale;  // "Chips:" width

                bool found_chip = false;
                for (int i = 0; i < vgm_info.num_chips && i < 4; i++) {
                    vgm_chip_info_t chip;
                    if (vgm_player_get_chip_info(i, &chip) == ESP_OK && chip.name) {
                        snprintf(vgm_line, sizeof(vgm_line), "[%s]", chip.name);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                chip_x, vgm_y, THEME_VU_MID, meta_scale, vgm_line);
                        chip_x += strlen(vgm_line) * FONT_WIDTH * meta_scale + 8;
                        found_chip = true;
                    }
                }
                if (!found_chip) {
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                            chip_x, vgm_y, THEME_TEXT_MUTED, meta_scale, "(detecting...)");
                }
                vgm_y += line_spacing + 16;

                // Progress bar - leave more room for time string
                int progress_bar_w = CONTENT_WIDTH - 200;
                int progress_bar_h = 16;
                int progress_bar_x = MARGIN_LEFT;
                int progress_bar_y = vgm_y;

                ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                              progress_bar_x, progress_bar_y, progress_bar_w, progress_bar_h,
                              THEME_VU_BG);

                double progress = 0.0;
                if (vgm_info.total_time_sec > 0) {
                    progress = vgm_info.current_time_sec / vgm_info.total_time_sec;
                    if (progress > 1.0) progress = 1.0;
                }
                int fill_w = (int)(progress_bar_w * progress);
                if (fill_w > 0) {
                    ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                  progress_bar_x, progress_bar_y, fill_w, progress_bar_h,
                                  THEME_VU_MID);
                }

                // Time display
                int cur_min = (int)(vgm_info.current_time_sec / 60);
                int cur_sec = (int)(vgm_info.current_time_sec) % 60;
                int tot_min = (int)(vgm_info.total_time_sec / 60);
                int tot_sec = (int)(vgm_info.total_time_sec) % 60;
                snprintf(vgm_line, sizeof(vgm_line), "%d:%02d / %d:%02d", cur_min, cur_sec, tot_min, tot_sec);
                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                        progress_bar_x + progress_bar_w + 8, progress_bar_y,
                                        THEME_TEXT_SECONDARY, 1, vgm_line);

                // Mono oscilloscope waveform visualization (L+R mixed)
                vgm_y = progress_bar_y + progress_bar_h + 24;
                int scope_w = CONTENT_WIDTH;
                int scope_h = 140;  // Taller single scope
                int scope_x = MARGIN_LEFT;
                int scope_y = vgm_y;
                int half_h = scope_h / 2;

                ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                              scope_x, scope_y, scope_w, scope_h, THEME_VU_BG);

                static int16_t wave_left[512];
                static int16_t wave_right[512];
                size_t wave_samples = 0;
                if (vgm_player_get_waveform(wave_left, wave_right, 512, &wave_samples) == ESP_OK && wave_samples > 0) {
                    int center_y = scope_y + half_h;
                    int prev_y = center_y;

                    // Find peak for auto-scaling
                    int32_t peak = 1;
                    for (size_t i = 0; i < wave_samples; i++) {
                        int32_t mixed = ((int32_t)wave_left[i] + (int32_t)wave_right[i]) / 2;
                        int32_t abs_val = (mixed < 0) ? -mixed : mixed;
                        if (abs_val > peak) peak = abs_val;
                    }
                    // Scale factor: map peak to 90% of half height
                    int scale_height = (half_h * 9) / 10;

                    for (size_t i = 0; i < wave_samples && i < (size_t)scope_w; i++) {
                        int x = scope_x + (int)(i * scope_w / wave_samples);
                        // Mix L+R for mono display
                        int32_t mixed = ((int32_t)wave_left[i] + (int32_t)wave_right[i]) / 2;
                        // Auto-scale based on peak
                        int y_offset = (int)((mixed * scale_height) / peak);
                        int y = center_y - y_offset;
                        if (y < scope_y) y = scope_y;
                        if (y >= scope_y + scope_h) y = scope_y + scope_h - 1;
                        if (i > 0) {
                            int y1 = (prev_y < y) ? prev_y : y;
                            int y2 = (prev_y > y) ? prev_y : y;
                            ui_draw_vline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, x, y1, y2 - y1 + 1, THEME_VU_LOW);
                        }
                        prev_y = y;
                    }

                    // Center line
                    ui_draw_hline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, scope_x, center_y, scope_w, THEME_TEXT_MUTED);
                } else {
                    // No data - just draw center line
                    ui_draw_hline(CURRENT_FB, FB_WIDTH, FB_HEIGHT, scope_x, scope_y + half_h, scope_w, THEME_TEXT_MUTED);
                }

                vgm_y = scope_y + scope_h + 8;

                if (vgm_info.has_loop) {
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                            MARGIN_LEFT, vgm_y, THEME_TEXT_MUTED, 1, "(Looping track)");
                    vgm_y += line_spacing;
                }

                // Footer hint bar
                const int vgm_hint_h = 20;
                int vgm_hint_y = FB_HEIGHT - MARGIN_BOTTOM - vgm_hint_h;
                ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                              0, vgm_hint_y, FB_WIDTH, vgm_hint_h, THEME_BG_PRIMARY);

                int vgm_icon_w = 16, vgm_icon_h = 16;
                if (fkey_icon_available(6)) fkey_icon_get_size(6, &vgm_icon_w, &vgm_icon_h);
                int vgm_gap = 20;
                int vgm_total = (vgm_icon_w + 4 + FONT_WIDTH * 4 + vgm_gap) +  // F6 Exit
                                (FONT_WIDTH * 6 + vgm_gap) +                   // arrows Vol
                                (FONT_WIDTH * 8);                              // space Pause
                int vgm_hx = (FB_WIDTH - vgm_total) / 2;
                int vgm_icon_y_pos = vgm_hint_y + (vgm_hint_h - vgm_icon_h) / 2;
                int vgm_text_y = vgm_hint_y + (vgm_hint_h - FONT_HEIGHT) / 2 + 1;

                if (fkey_icon_available(6)) {
                    fkey_icon_draw(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_icon_y_pos, 6, 1);
                    vgm_hx += vgm_icon_w + 4;
                } else {
                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, "F6");
                    vgm_hx += FONT_WIDTH * 2 + 4;
                }
                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, "Exit");
                vgm_hx += FONT_WIDTH * 4 + vgm_gap;

                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, "\x80\x81 Vol");
                vgm_hx += FONT_WIDTH * 6 + vgm_gap;

                const char *vgm_pause = vgm_player_is_paused() ? "\x85 Resume" : "\x85 Pause";
                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, vgm_hx, vgm_text_y, THEME_TEXT_MUTED, 1, vgm_pause);

                PROFILING_END(render, render);
                did_render = true;
            }
        }

        // Only call blit() if we actually rendered something this frame
        // This prevents unnecessary rotation and display updates when nothing has changed
        if (did_render) {
            blit(-1, 0);
        }
        
        PROFILING_END_FRAME(frame);
        if (g_profiling_stats.frame_count % 10 == 0) {
            profiling_report();
        }
    }
}
