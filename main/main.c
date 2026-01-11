#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include "audio.h"
#include "mod_player.h"
#include "sdcard.h"
#include "file_browser.h"
#include "xmp.h"  // For xmp_frame_info, xmp_channel_info, xmp_event
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
#include "wifi_connection.h"
#include "wifi_remote.h"

// Constants
static char const TAG[] = "main";

// Logical landscape framebuffer dimensions (matches physical display orientation)
// Will be rotated 270° CCW via PPA when sending to ST7701S (native 480x800 portrait)
#define FB_WIDTH  800
#define FB_HEIGHT 480

// Screen margins to ensure content is visible
#define MARGIN_LEFT   10
#define MARGIN_RIGHT  10
#define MARGIN_TOP    5
#define MARGIN_BOTTOM 5

// Effective content area after margins
#define CONTENT_WIDTH  (FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT)   // 780
#define CONTENT_HEIGHT (FB_HEIGHT - MARGIN_TOP - MARGIN_BOTTOM)  // 470

// RGB565 color conversion helper (ARGB32 to RGB565)
// Format: ARGB32 = 0xAARRGGBB, RGB565 = 0bRRRRRGGGGGGBBBBB
static inline uint16_t argb32_to_rgb565(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Global variables
static uint16_t *fb = NULL;  // Logical landscape framebuffer (800x480) in PSRAM
static uint16_t *fb_rotated = NULL;  // Rotated framebuffer (480x800) for display output
static esp_lcd_panel_handle_t panel_handle = NULL;  // Panel handle for direct drawing
static ppa_client_handle_t ppa_srm_handle = NULL;  // PPA SRM client for rotation
static QueueHandle_t input_event_queue = NULL;

// RGB565 color constants
#define RGB565_BLACK   0x0000
#define RGB565_WHITE   0xFFFF
#define RGB565_RED     0xF800
#define RGB565_GREEN   0x07E0
#define RGB565_BLUE    0x001F
#define RGB565_YELLOW  0xFFE0
#define RGB565_MAGENTA 0xF81F
#define RGB565_CYAN    0x07FF

// KAMI support removed - Tanmatsu only

// Helper function to draw file browser view
static void draw_file_browser(file_browser_t *browser) {
    const int font_scale = 2;
    const int line_height = FONT_HEIGHT * font_scale;  // 16 pixels for scale 2
    
    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    
    int y = MARGIN_TOP;
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, y, RGB565_WHITE, font_scale, "MOD file browser");
    y += line_height;
    
    char path_text[128];
    int path_len = snprintf(path_text, sizeof(path_text), "Path: %s", browser->current_path);
    if (path_len >= (int)sizeof(path_text)) {
        path_text[sizeof(path_text) - 1] = '\0';
    }
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, y, RGB565_WHITE, font_scale, path_text);
    y += line_height + 2;  // Small gap before file list
    
    // Draw file list (show up to 10 files, centered on selection)
    int start_idx = browser->selected_index > 5 ? browser->selected_index - 5 : 0;
    int end_idx = start_idx + 10;
    if (end_idx > browser->count) end_idx = browser->count;
    
    int file_list_start_y = y;
    for (int i = start_idx; i < end_idx; i++) {
        int file_y = file_list_start_y + (i - start_idx) * line_height;
        if (i == browser->selected_index) {
            // Draw blue highlight - match text height exactly (16 pixels for scale 2)
            fb_rect(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, file_y, CONTENT_WIDTH, line_height, argb32_to_rgb565(0xFF0000FF));
        }
        char name[64];
        // Special handling for ".." entry - ensure it displays correctly
        if (strcmp(browser->files[i].filename, "..") == 0) {
            snprintf(name, sizeof(name), "[..]");
        } else {
            int name_len = snprintf(name, sizeof(name), "%s%s", browser->files[i].is_dir ? "[" : "", browser->files[i].filename);
            if (name_len >= (int)sizeof(name)) {
                name[sizeof(name) - 1] = '\0';
            }
            if (browser->files[i].is_dir) {
                int len = strlen(name);
                if (len < (int)sizeof(name) - 1) {
                    name[len] = ']';
                    name[len + 1] = '\0';
                }
            }
        }
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT + 4, file_y, RGB565_WHITE, font_scale, name);
    }
    
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, FB_HEIGHT - MARGIN_BOTTOM - line_height, RGB565_WHITE, font_scale, "UP/DN: navigate  RT/ENT: select  LT: back");
}

void blit(void) {
    // Use PPA to rotate logical landscape framebuffer (800x480) to ST7701S native portrait (480x800)
    if (fb && fb_rotated && ppa_srm_handle && panel_handle) {
        // Configure PPA SRM operation: rotate 270° CCW (landscape -> portrait)
        ppa_srm_oper_config_t srm_config = {
            .in.buffer = fb,
            .in.pic_w = FB_WIDTH,      // 800
            .in.pic_h = FB_HEIGHT,     // 480
            .in.block_w = FB_WIDTH,
            .in.block_h = FB_HEIGHT,
            .in.block_offset_x = 0,
            .in.block_offset_y = 0,
            .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .out.buffer = fb_rotated,
            .out.buffer_size = 480 * 800 * sizeof(uint16_t),  // Rotated size
            .out.pic_w = 480,  // After 270° rotation: height becomes width
            .out.pic_h = 800,  // After 270° rotation: width becomes height
            .out.block_offset_x = 0,
            .out.block_offset_y = 0,
            .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,  // 270° CCW rotation
            .scale_x = 1,
            .scale_y = 1,
            .rgb_swap = 0,
            .byte_swap = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        
        // Perform rotation
        esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
        if (ret == ESP_OK) {
            // Send rotated buffer directly to panel
            esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 480, 800, fb_rotated);
        } else {
            ESP_LOGE(TAG, "PPA rotation failed: %s", esp_err_to_name(ret));
        }
    }
}

void app_main(void) {
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
    const bsp_configuration_t bsp_configuration = {
        .display =
            {
                .requested_color_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&bsp_configuration));
    
    // Get panel handle for direct PPA operations
    ESP_ERROR_CHECK(bsp_display_get_panel(&panel_handle));
    
    // Initialize PPA SRM client for rotation
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    ESP_LOGI(TAG, "PPA SRM client registered for rotation");

    // Skip LED initialization for faster startup (can re-enable later if needed)
    // bsp_led_set_pixel(0, 0xFF0000);  // Red
    // bsp_led_set_pixel(1, 0x00FF00);  // Green
    // bsp_led_set_pixel(2, 0x0000FF);  // Blue
    // bsp_led_set_pixel(3, 0xFFFF00);  // Yellow
    // bsp_led_set_pixel(4, 0x00FFFF);  // Magenta
    // bsp_led_set_pixel(5, 0xFF00FF);  // Cyan
    // bsp_led_send();                  // Send data to the coprocessor
    // bsp_led_set_mode(false);         // Take control over all LEDs by disabling automatic mode

    
    // Allocate logical landscape framebuffer (800x480) from DMA-capable PSRAM
    // DMA requires 32-byte alignment for optimal performance
    size_t fb_size = FB_WIDTH * FB_HEIGHT * sizeof(uint16_t);  // 800x480x2 = 768000 bytes
    fb = (uint16_t*)heap_caps_aligned_alloc(32, fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate logical framebuffer from DMA-capable PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Allocated logical framebuffer: %zu bytes (800x480)", fb_size);
    
    // Allocate rotated framebuffer (480x800) for PPA output
    size_t fb_rotated_size = 480 * 800 * sizeof(uint16_t);  // 480x800x2 = 768000 bytes
    fb_rotated = (uint16_t*)heap_caps_aligned_alloc(32, fb_rotated_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (fb_rotated == NULL) {
        ESP_LOGE(TAG, "Failed to allocate rotated framebuffer from DMA-capable PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Allocated rotated framebuffer: %zu bytes (480x800)", fb_rotated_size);

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    // Initialize audio system
    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_BLACK, 2, "Initializing audio...");
    blit();
    res = audio_init();
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Audio initialized successfully");
        
        // Initialize MOD player first (task will start but won't play until MOD is loaded)
        res = mod_player_init(44100);
        
        // Small delay to let MOD player task initialize
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Startup beep removed
        // audio_beep(100);
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "MOD player initialized");
        } else {
            ESP_LOGW(TAG, "MOD player initialization failed: %s", esp_err_to_name(res));
        }
    } else {
        ESP_LOGW(TAG, "Audio initialization failed: %s", esp_err_to_name(res));
        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Audio init failed");
        blit();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Skip WiFi initialization for faster startup (can re-enable later if needed)
    //
    // if (wifi_remote_initialize() == ESP_OK) {
    //
    //     fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //     pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Starting WiFi stack...");
    //     blit();
    //     wifi_connection_init_stack();  // Start the Espressif WiFi stack
    //
    //     fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //     pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Connecting to WiFi network...");
    //     blit();
    //
    //     if (wifi_connect_try_all() == ESP_OK) {
    //         fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //         pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Succesfully connected to WiFi network");
    //         blit();
    //     } else {
    //         pax_background(&fb, RED);
    //         pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "Failed to connect to WiFi network");
    //         blit();
    //     }
    // } else {
    //     bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    //     ESP_LOGE(TAG, "WiFi radio not responding, WiFi not available");
    //     pax_background(&fb, RED);
    //     pax_draw_text(&fb, WHITE, pax_font_sky_mono, 16, 0, 0, "WiFi unavailable");
    //     blit();
    // }
    //
    // vTaskDelay(pdMS_TO_TICKS(500));

    // Initialize SD card
    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_BLACK, 2, "Initializing SD card...");
    blit();
    res = sdcard_init();
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s", esp_err_to_name(res));
        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "SD card init failed");
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 18, RGB565_BLACK, 2, "Continuing without SD");
        blit();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Main section of the app

    // This example shows how to read from the BSP event queue to read input events

    // If you want to run something at an interval in this same main thread you can replace portMAX_DELAY with an amount
    // of ticks to wait, for example pdMS_TO_TICKS(1000)

    // File browser state (static to avoid stack overflow)
    static file_browser_t browser = {0};
    static bool browser_active = false;
    static uint8_t *mod_file_data = NULL;
    static size_t mod_file_size = 0;
    static char current_mod_path[MAX_FILENAME_LEN] = {0};  // Store current MOD file path for display

    // Start with file browser if SD card is mounted
    if (sdcard_is_mounted()) {
        browser_active = true;
        file_browser_init(&browser, "/sdcard");
        // Draw initial file browser view using helper function (with margins)
        draw_file_browser(&browser);
        blit();
    } else {
        // No SD card - show error
        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "SD card not mounted");
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 18, RGB565_BLACK, 2, "Insert SD card and");
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 36, RGB565_BLACK, 2, "restart device");
    blit();
    }

    // Tracker UI state - scrolling view with one line per pattern row
    #define MAX_TRACKER_ROWS 60  // Maximum number of row lines to store (more than screen can display for scrolling)
    struct tracker_tick {
        struct xmp_channel_info channels[XMP_MAX_CHANNELS];
        int pos;
        int pattern;
        int row;
        int num_channels;
    };
    static struct tracker_tick tick_history[MAX_TRACKER_ROWS] = {0};
    static int tick_history_count = 0;
    static int last_row = -1;
    static struct xmp_module_info mod_info = {0};
    static bool mod_info_loaded = false;
    
    // Channel colors (RGB565)
    static const uint16_t channel_colors_rgb565[XMP_MAX_CHANNELS] = {
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

    while (1) {
        // Check input queue FIRST with ZERO timeout for instant response (non-blocking)
        bsp_input_event_t event;
        // Process ALL pending input events before doing anything else
        while (xQueueReceive(input_event_queue, &event, 0) == pdTRUE) {
            // Process input immediately
            switch (event.type) {
                case INPUT_EVENT_TYPE_NAVIGATION: {
                    // Handle navigation keys (UP/DOWN/LEFT/RIGHT) for file browser and volume control
                    if (event.args_navigation.state) {
                        // Only process key press (not release)
                        if (browser_active) {
                            bool browser_needs_redraw = false;
                            
                            switch (event.args_navigation.key) {
                                case BSP_INPUT_NAVIGATION_KEY_UP:
                                    file_browser_up(&browser);
                                    browser_needs_redraw = true;
                                    break;
                                case BSP_INPUT_NAVIGATION_KEY_DOWN:
                                    file_browser_down(&browser);
                                    browser_needs_redraw = true;
                                    break;
                            case BSP_INPUT_NAVIGATION_KEY_LEFT:
                                if (file_browser_back(&browser) == ESP_OK) {
                                    browser_needs_redraw = true;
                                }
                                break;
                            case BSP_INPUT_NAVIGATION_KEY_RIGHT: {
                                // Enter directory or select file
                                bool is_directory = false;
                                if (browser.selected_index < browser.count) {
                                    is_directory = browser.files[browser.selected_index].is_dir;
                                }
                                
                                static char selected_path[MAX_FILENAME_LEN];
                                esp_err_t select_res = file_browser_select(&browser, selected_path, sizeof(selected_path));
                                if (select_res == ESP_OK && !is_directory) {
                                    // File selected - check if it's a MOD file
                                    const char *ext = strrchr(selected_path, '.');
                                    if (ext && (strcasecmp(ext, ".mod") == 0 || strcasecmp(ext, ".xm") == 0 ||
                                                strcasecmp(ext, ".s3m") == 0 || strcasecmp(ext, ".it") == 0)) {
                                        // Valid MOD file - load it
                                        FILE *f = fopen(selected_path, "rb");
                                        if (f) {
                                            fseek(f, 0, SEEK_END);
                                            long file_size = ftell(f);
                                            fseek(f, 0, SEEK_SET);
                                            
                                            // Free previous MOD data if any
                                            if (mod_file_data) {
                                                free(mod_file_data);
                                            }
                                            
                                            mod_file_data = (uint8_t *)malloc(file_size);
                                            if (mod_file_data) {
                                                size_t read = fread(mod_file_data, 1, file_size, f);
                                                fclose(f);
                                                
                                                if (read == file_size) {
                                                    mod_file_size = file_size;
                                                    // Store file path for display
                                                    strncpy(current_mod_path, selected_path, sizeof(current_mod_path) - 1);
                                                    current_mod_path[sizeof(current_mod_path) - 1] = '\0';
                                                    res = mod_player_load(mod_file_data, mod_file_size);
                                                    if (res == ESP_OK) {
                                                        res = mod_player_start();
                                                    if (res == ESP_OK) {
                                                        browser_active = false;
                                                        mod_info_loaded = false;  // Force reload of module info
                                                        // Clear screen immediately to prevent white flash
                                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        // Tracker UI will be drawn in main loop
                                                    } else {
                                                    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                            blit();
                                                        }
                                                    } else {
                                                        free(mod_file_data);
                                                        mod_file_data = NULL;
                                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                        blit();
                                                    }
                                                } else {
                                                    free(mod_file_data);
                                                    mod_file_data = NULL;
                                                    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                                    blit();
                                                }
                                            } else {
                                                fclose(f);
                                                fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                                blit();
                                            }
                                        } else {
                                            fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                        blit();
                    }
                                    }
                                } else if (select_res == ESP_OK) {
                                    // Directory entered - browser already refreshed
                                    browser_needs_redraw = true;
                                }
                                break;
                            }
                            default:
                    break;
                }
                        
                        // Redraw file browser if selection changed
                        if (browser_needs_redraw) {
                            draw_file_browser(&browser);
                            blit();
                        }
                        } else if (mod_player_is_playing()) {
                            // Volume control during playback
                            float current_vol = 0.0f;
                            if (audio_get_volume(&current_vol) == ESP_OK) {
                                float new_vol = current_vol;
                                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
                                    new_vol += 0.05f;  // Increase by 5%
                                    if (new_vol > 0.90f) new_vol = 0.90f;  // Cap at 90%
                                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                                    new_vol -= 0.05f;  // Decrease by 5%
                                    if (new_vol < 0.30f) new_vol = 0.30f;  // Cap at 30%
                                }
                                audio_set_volume(new_vol);
                            }
                        }
                    }
                    break;
                }
                case INPUT_EVENT_TYPE_KEYBOARD:
                    // Keyboard events not currently used
                    break;
                case INPUT_EVENT_TYPE_ACTION: {
                    // Power button handler removed - using F6 instead
                    // Debug: Action event (commented out)
                    break;
                }
                case INPUT_EVENT_TYPE_SCANCODE: {
                    // ESP_LOGI(TAG, "Scancode event 0x%0" PRIX32, (uint32_t)event.args_scancode.scancode);
                    
                    bsp_input_scancode_t sc = event.args_scancode.scancode;
                    
                    // Handle F6 key (0x40) for exit
                    if (sc == 0x40) {
                        if (mod_player_is_playing()) {
                            mod_player_stop();
                        }
                        if (browser_active) {
                            audio_stop();
                            audio_set_volume(0.0f);
                            vTaskDelay(pdMS_TO_TICKS(50));
                            bsp_device_restart_to_launcher();
                        } else {
                            // During playback - return to file browser
                            mod_player_stop();
                            browser_active = true;
                            file_browser_refresh(&browser);
                            // Redraw file browser
                            draw_file_browser(&browser);
                            blit();
                        }
                    }
                    
                    // Handle arrow keys via scancode (fallback for navigation)
                    if (browser_active) {
                        bool browser_needs_redraw = false;
                        bsp_input_scancode_t sc = event.args_scancode.scancode;
                        
                        // Arrow key scancodes (standard PC scancodes)
                        if (sc == 0x48) {  // Up arrow
                            file_browser_up(&browser);
                            browser_needs_redraw = true;
                        } else if (sc == 0x50) {  // Down arrow
                            file_browser_down(&browser);
                            browser_needs_redraw = true;
                        } else if (sc == 0x4B) {  // Left arrow
                            if (file_browser_back(&browser) == ESP_OK) {
                                browser_needs_redraw = true;
                            }
                        } else if (sc == 0x4D) {  // Right arrow
                            // Enter directory or select file (same as Enter key)
                            bool is_directory = false;
                            if (browser.selected_index < browser.count) {
                                is_directory = browser.files[browser.selected_index].is_dir;
                            }
                            
                            static char selected_path[MAX_FILENAME_LEN];
                            esp_err_t select_res = file_browser_select(&browser, selected_path, sizeof(selected_path));
                            if (select_res == ESP_OK && !is_directory) {
                                // File selected - check if it's a MOD file
                                const char *ext = strrchr(selected_path, '.');
                                if (ext && (strcasecmp(ext, ".mod") == 0 || strcasecmp(ext, ".xm") == 0 ||
                                            strcasecmp(ext, ".s3m") == 0 || strcasecmp(ext, ".it") == 0)) {
                                    // Valid MOD file - load it
                                    FILE *f = fopen(selected_path, "rb");
                                    if (f) {
                                        fseek(f, 0, SEEK_END);
                                        long file_size = ftell(f);
                                        fseek(f, 0, SEEK_SET);
                                        
                                        if (mod_file_data) {
                                            free(mod_file_data);
                                        }
                                        
                                        mod_file_data = (uint8_t *)malloc(file_size);
                                        if (mod_file_data) {
                                            size_t read = fread(mod_file_data, 1, file_size, f);
                                            fclose(f);
                                            
                                            if (read == file_size) {
                                                mod_file_size = file_size;
                                                // Store file path for display
                                                strncpy(current_mod_path, selected_path, sizeof(current_mod_path) - 1);
                                                current_mod_path[sizeof(current_mod_path) - 1] = '\0';
                                                res = mod_player_load(mod_file_data, mod_file_size);
                                                if (res == ESP_OK) {
                                                    res = mod_player_start();
                                                    if (res == ESP_OK) {
                                                        browser_active = false;
                                                        mod_info_loaded = false;  // Force reload of module info
                                                        // Clear screen immediately to prevent white flash
                                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        // Tracker UI will be drawn in main loop
                                                        browser_needs_redraw = false;  // Don't redraw browser, we're playing now
                                                    } else {
                                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                        blit();
                                                    }
                                                } else {
                                                    free(mod_file_data);
                                                    mod_file_data = NULL;
                                                    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                    blit();
                                                }
                                            } else {
                                                free(mod_file_data);
                                                mod_file_data = NULL;
                                                fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                                blit();
                                            }
                                        } else {
                                            fclose(f);
                                            fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                            blit();
                                        }
                                    } else {
                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                                        blit();
                                    }
                                }
                            } else if (select_res == ESP_OK && is_directory) {
                                // Directory entered - browser already refreshed by file_browser_select()
                                browser_needs_redraw = true;
                            }
                        }
                        
                        // Redraw file browser if navigation changed
                        if (browser_needs_redraw) {
                            draw_file_browser(&browser);
                            blit();
                        }
                    }
                    
                    // Handle Enter key (0x1C press) for file selection in browser
                    if (browser_active && event.args_scancode.scancode == 0x1C) {
                        // Enter key pressed - same as RIGHT arrow for selection
                        bool is_directory = false;
                        if (browser.selected_index < browser.count) {
                            is_directory = browser.files[browser.selected_index].is_dir;
                        }
                        
                        bool browser_needs_redraw = false;
                        static char selected_path[MAX_FILENAME_LEN];
                        esp_err_t select_res = file_browser_select(&browser, selected_path, sizeof(selected_path));
                        if (select_res == ESP_OK && !is_directory) {
                            // File selected - check if it's a MOD file
                            const char *ext = strrchr(selected_path, '.');
                            if (ext && (strcasecmp(ext, ".mod") == 0 || strcasecmp(ext, ".xm") == 0 ||
                                        strcasecmp(ext, ".s3m") == 0 || strcasecmp(ext, ".it") == 0)) {
                                // Valid MOD file - load it (same logic as RIGHT arrow handler)
                                FILE *f = fopen(selected_path, "rb");
                                if (f) {
                                    fseek(f, 0, SEEK_END);
                                    long file_size = ftell(f);
                                    fseek(f, 0, SEEK_SET);
                                    
                                    if (mod_file_data) {
                                        free(mod_file_data);
                                    }
                                    
                                    mod_file_data = (uint8_t *)malloc(file_size);
                                    if (mod_file_data) {
                                        size_t read = fread(mod_file_data, 1, file_size, f);
                                        fclose(f);
                                        
                                        if (read == file_size) {
                                            mod_file_size = file_size;
                                            // Store file path for display
                                            strncpy(current_mod_path, selected_path, sizeof(current_mod_path) - 1);
                                            current_mod_path[sizeof(current_mod_path) - 1] = '\0';
                                            res = mod_player_load(mod_file_data, mod_file_size);
                                            if (res == ESP_OK) {
                                                res = mod_player_start();
                                                    if (res == ESP_OK) {
                                                        browser_active = false;
                                                        mod_info_loaded = false;  // Force reload of module info
                                                        // Clear screen immediately to prevent white flash
                                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        // Tracker UI will be drawn in main loop
                                                    } else {
                                                    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                    blit();
                                                }
                                            } else {
                                                free(mod_file_data);
                                                mod_file_data = NULL;
                                                fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                blit();
                                            }
                                        } else {
                                            free(mod_file_data);
                                            mod_file_data = NULL;
                                            fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                            blit();
                                        }
                                    } else {
                                        fclose(f);
                                        fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                        blit();
                                    }
                                } else {
                                    fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                                    blit();
                                }
                            }
                        } else if (select_res == ESP_OK && is_directory) {
                            // Directory entered - browser already refreshed by file_browser_select()
                            browser_needs_redraw = true;
                        }
                        
                        // Redraw file browser if directory was entered
                        if (browser_needs_redraw) {
                            draw_file_browser(&browser);
                            blit();
                        }
                    }
                    
                    // Debug: Scancode event (commented out)
                    break;
                }
                default:
                    break;
            }
        }  // End of while loop processing all pending input events
        
        // Render tracker UI if playing (frame rate limited - only update on row changes)
        if (mod_player_is_playing() && !browser_active) {
            struct xmp_frame_info frame_info;
            if (mod_player_get_frame_info(&frame_info) == ESP_OK) {
                // Only render when row changes (frame rate limiting)
                if (frame_info.row != last_row) {
                    // Load module info if not loaded
                    if (!mod_info_loaded) {
                        mod_player_get_module_info(&mod_info);
                        mod_info_loaded = true;
                    }
                    
                    int num_channels = mod_info.mod ? mod_info.mod->chn : 0;
                    if (num_channels > 0 && num_channels <= XMP_MAX_CHANNELS) {
                        // Calculate layout (cache for performance)
                        static int cached_line_height = 0;
                        static int cached_max_rows = 0;
                        static int cached_ch_width = 0;
                        static int cached_num_channels = 0;
                        
                        // Recalculate if channel count changed
                        if (cached_num_channels != num_channels) {
                            // Dynamic text scaling to use ~85% of content width (after margins: 780px)
                            const int content_width = CONTENT_WIDTH;  // 780 (800 - 10 - 10)
                            const int content_height = CONTENT_HEIGHT;  // 470 (480 - 5 - 5)
                            int available_width = (int)(content_width * 0.85);
                            int ch_width_estimate = available_width / num_channels;
                            // Scale line height from 10px to 18px based on channel width
                            cached_line_height = 10 + (ch_width_estimate - 40) * 8 / 200;
                            if (cached_line_height < 10) cached_line_height = 10;
                            if (cached_line_height > 18) cached_line_height = 18;
                            cached_max_rows = (int)(content_height / cached_line_height);
                            cached_ch_width = ch_width_estimate;
                            cached_num_channels = num_channels;
                        }
                        
                        int line_height = cached_line_height;
                        int max_rows_on_screen = cached_max_rows;
                        int ch_width = cached_ch_width;
                        
                        // Track if this is the first render (need full screen)
                        static bool tracker_initialized = false;
                        // Reset tracker_initialized when module info is reloaded (new MOD started)
                        if (!mod_info_loaded) {
                            tracker_initialized = false;
                        }
                        int header_y = MARGIN_TOP + FONT_HEIGHT * 2 + 2;  // Leave space for header (margin + text + gap)
                        
                        // Get direct framebuffer access for scrolling (RGB565 = 2 bytes per pixel)
                        // Logical landscape framebuffer: 800 pixels wide, 480 pixels tall
                        uint16_t *fb_pixels = fb;  // Direct framebuffer access
                        const int logical_width = FB_WIDTH;  // 800
                        const int logical_height = FB_HEIGHT;  // 480
                        int stride = logical_width;  // Pixels per row in landscape framebuffer (800)
                        
                        // Hex digit lookup table removed - using snprintf with %02X format instead
                        
                        // Note name lookup
                        static const char *note_names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
                        
                        // Store current frame data
                        if (tick_history_count < MAX_TRACKER_ROWS) {
                            memcpy(tick_history[tick_history_count].channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                            tick_history[tick_history_count].pos = frame_info.pos;
                            tick_history[tick_history_count].pattern = frame_info.pattern;
                            tick_history[tick_history_count].row = frame_info.row;
                            tick_history[tick_history_count].num_channels = num_channels;
                            tick_history_count++;
                        } else {
                            // Shift history
                            memmove(tick_history, tick_history + 1, (MAX_TRACKER_ROWS - 1) * sizeof(struct tracker_tick));
                            memcpy(tick_history[MAX_TRACKER_ROWS - 1].channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                            tick_history[MAX_TRACKER_ROWS - 1].pos = frame_info.pos;
                            tick_history[MAX_TRACKER_ROWS - 1].pattern = frame_info.pattern;
                            tick_history[MAX_TRACKER_ROWS - 1].row = frame_info.row;
                            tick_history[MAX_TRACKER_ROWS - 1].num_channels = num_channels;
                        }
                        
                        if (!tracker_initialized) {
                            // First render - full screen
                            fb_fill(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                            
                            // Display MOD file name and volume at the top
                            float current_vol = 0.0f;
                            if (audio_get_volume(&current_vol) == ESP_OK) {
                                char header_text[128];
                                const char *filename = strrchr(current_mod_path, '/');
                                if (!filename) filename = current_mod_path;
                                else filename++;
                                if (!filename[0]) filename = "Unknown";
                                
                                int vol_percent = (int)(current_vol * 100.0f);
                                int header_len = snprintf(header_text, sizeof(header_text), "%s | Vol: %d%%", filename, vol_percent);
                                if (header_len >= (int)sizeof(header_text)) {
                                    header_text[sizeof(header_text) - 1] = '\0';
                                }
                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, MARGIN_TOP, RGB565_WHITE, 2, header_text);
                            }
                            
                            // Render all visible rows
                            int rows_to_show = (tick_history_count < max_rows_on_screen) ? max_rows_on_screen : tick_history_count;
                            if (rows_to_show > max_rows_on_screen) rows_to_show = max_rows_on_screen;
                            int start_row = (tick_history_count > max_rows_on_screen) ? (tick_history_count - max_rows_on_screen) : 0;
                            
                            for (int row_idx = 0; row_idx < rows_to_show; row_idx++) {
                                int y = header_y + row_idx * line_height;
                                int hist_idx = start_row + row_idx;
                                
                                if (hist_idx < tick_history_count) {
                                    struct tracker_tick *tick = &tick_history[hist_idx];
                                    
                                    int x = MARGIN_LEFT;
                                    for (int ch = 0; ch < num_channels && ch < XMP_MAX_CHANNELS; ch++) {
                                        struct xmp_channel_info *ci = &tick->channels[ch];
                                        
                                        char ch_str[64];
                                        unsigned char note = ci->event.note;
                                        unsigned char ins = ci->event.ins;
                                        unsigned char fxt = ci->event.fxt;
                                        unsigned char fxp = ci->event.fxp;
                                        
                                        char note_str[4];
                                        if (note > 0 && note <= 96) {
                                            int note_idx = (note - 1) % 12;
                                            int octave = (note - 1) / 12;
                                            if (note_idx < 0) note_idx = 0;
                                            if (note_idx > 11) note_idx = 11;
                                            if (octave < 0) octave = 0;
                                            if (octave > 9) octave = 9;
                                            snprintf(note_str, sizeof(note_str), "%s%d", note_names[note_idx], octave);
                                        } else {
                                            strcpy(note_str, "---");
                                        }
                                        
                                        char effect_char;
                                        if (fxt == 0) {
                                            effect_char = '-';
                                        } else if (fxt < 10) {
                                            effect_char = '0' + fxt;
                                        } else if (fxt < 16) {
                                            effect_char = 'A' + (fxt - 10);
                                        } else {
                                            effect_char = '?';
                                        }
                                        
                                        snprintf(ch_str, sizeof(ch_str), "%s %02X %c%02X", note_str, ins, effect_char, fxp);
                                        
                                    uint16_t ch_color = channel_colors_rgb565[ch % XMP_MAX_CHANNELS];
                                    // Scale factor: line_height / FONT_HEIGHT (typically 14-18 / 8 = 1-2)
                                    int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                    if (font_scale < 1) font_scale = 1;
                                    if (font_scale > 3) font_scale = 3;
                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                    
                                    x += ch_width;
                                    
                                    if (ch < num_channels - 1) {
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, "  |  ");
                                        x += line_height * 3;
                                    }
                                    }
                                } else {
                                    // Empty row placeholder - use same font scale as regular rows
                                    int x = MARGIN_LEFT;
                                    int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                    if (font_scale < 1) font_scale = 1;
                                    if (font_scale > 3) font_scale = 3;
                                    for (int ch = 0; ch < num_channels && ch < XMP_MAX_CHANNELS; ch++) {
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, "--- -- ---");
                                        x += ch_width;
                                        if (ch < num_channels - 1) {
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, "  |  ");
                                            x += line_height * 3;
                                        }
                                    }
                                }
                            }
                            
                            tracker_initialized = true;
                            // Full screen blit on first render
                            blit();
                        } else {
                            // Subsequent renders - scroll content up and only draw new row
                            // Scroll framebuffer content up (skip header row at top, account for margins)
                            int scrollable_height = FB_HEIGHT - header_y - MARGIN_BOTTOM;
                            int scroll_lines = line_height;
                            int src_offset = (header_y + scroll_lines) * stride;
                            int dst_offset = header_y * stride;
                            int copy_height = scrollable_height - scroll_lines;
                            
                            // Scroll the content region up using memmove
                            if (copy_height > 0) {
                                memmove(fb_pixels + dst_offset, fb_pixels + src_offset, copy_height * stride * sizeof(uint16_t));
                            }
                            
                            // Clear the bottom line_height rows (where new content will go, account for margin)
                            int clear_y = FB_HEIGHT - MARGIN_BOTTOM - scroll_lines;
                            memset(fb_pixels + clear_y * stride, 0, scroll_lines * stride * sizeof(uint16_t));
                            
                            // Only draw the new row at the bottom (account for margin)
                            int y = logical_height - MARGIN_BOTTOM - line_height;
                            int hist_idx = tick_history_count - 1;
                            
                            if (hist_idx >= 0 && hist_idx < tick_history_count) {
                                struct tracker_tick *tick = &tick_history[hist_idx];
                                
                                int x = MARGIN_LEFT;
                                for (int ch = 0; ch < num_channels && ch < XMP_MAX_CHANNELS; ch++) {
                                    struct xmp_channel_info *ci = &tick->channels[ch];
                                    
                                    char ch_str[64];
                                    unsigned char note = ci->event.note;
                                    unsigned char ins = ci->event.ins;
                                    unsigned char fxt = ci->event.fxt;
                                    unsigned char fxp = ci->event.fxp;
                                    
                                    char note_str[4];
                                    if (note > 0 && note <= 96) {
                                        int note_idx = (note - 1) % 12;
                                        int octave = (note - 1) / 12;
                                        if (note_idx < 0) note_idx = 0;
                                        if (note_idx > 11) note_idx = 11;
                                        if (octave < 0) octave = 0;
                                        if (octave > 9) octave = 9;
                                        snprintf(note_str, sizeof(note_str), "%s%d", note_names[note_idx], octave);
                                    } else {
                                        strcpy(note_str, "---");
                                    }
                                    
                                    char effect_char;
                                    if (fxt == 0) {
                                        effect_char = '-';
                                    } else if (fxt < 10) {
                                        effect_char = '0' + fxt;
                                    } else if (fxt < 16) {
                                        effect_char = 'A' + (fxt - 10);
                                    } else {
                                        effect_char = '?';
                                    }
                                    
                                    snprintf(ch_str, sizeof(ch_str), "%s %02X %c%02X", note_str, ins, effect_char, fxp);
                                    
                                    uint16_t ch_color = channel_colors_rgb565[ch % XMP_MAX_CHANNELS];
                                    // Scale factor: line_height / FONT_HEIGHT (typically 14-18 / 8 = 1-2)
                                    int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                    if (font_scale < 1) font_scale = 1;
                                    if (font_scale > 3) font_scale = 3;
                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                    
                                    x += ch_width;
                                    
                                    if (ch < num_channels - 1) {
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, "  |  ");
                                        x += line_height * 3;
                                    }
                                }
                            }
                            
                            // Partial blit - use full blit for now (can optimize later with partial PPA rotation)
                            // PPA will rotate the entire logical framebuffer and send to display
                            blit();
                        }
                    }
                    
                    last_row = frame_info.row;
                }
            }
        }
    }
}
