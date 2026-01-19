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
#include "esp_async_memcpy.h"  // For GDMA memory operations
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

// Constants
static char const TAG[] = "main";

// Internal flash wear levelling handle
static wl_handle_t int_flash_wl_handle = WL_INVALID_HANDLE;

// Global framebuffers and hardware handles
static uint16_t *fb = NULL;  // Logical landscape framebuffer (800x480)
static uint16_t *fb_rotated = NULL;  // Rotated framebuffer (480x800)
static ppa_client_handle_t ppa_srm_handle = NULL;  // PPA SRM client
static ppa_client_handle_t ppa_fill_handle = NULL;  // PPA Fill client
static QueueHandle_t input_event_queue = NULL;  // Input event queue

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
#define CURRENT_FB (fb)



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
    hw_accel_blit(NULL, fb, fb_rotated, row_y, row_height);
}

static inline void draw_file_browser(file_browser_t *browser) {
    ui_draw_file_browser(fb, FB_WIDTH, FB_HEIGHT, browser);
}

static inline void format_channel_string(char *ch_str, size_t ch_str_size,
                                         unsigned char note, unsigned char ins,
                                         unsigned char fxt, unsigned char fxp,
                                         bool use_compact, const char *note_names[12]) {
    ui_format_channel_string(ch_str, ch_str_size, note, ins, fxt, fxp, use_compact, note_names);
}

static volatile bool gdma_copy_done = false;

static bool gdma_memcpy_callback(async_memcpy_handle_t mcp_hdl, async_memcpy_event_t *event, void *cb_args) {
    (void)mcp_hdl;
    (void)event;
    (void)cb_args;
    gdma_copy_done = true;
    return false;  // No high priority task woken
}

// Hardware-accelerated scrolling using PPA SRM with 0° rotation
// Optimized: Direct copy from source to destination (no temp buffer needed)
// Since src_y > dst_y (scrolling up), regions don't overlap, so we can copy directly

// Blit function - rotates framebuffer and sends to display via BSP
// BSP handles vsync and double buffering internally

#if 0
// Old blit implementation - removed, LCD task now handles rotation and display
void IRAM_ATTR blit_old(int row_y, int row_height) {
    // Use PPA to rotate logical landscape framebuffer (800x480) to ST7701S native portrait (480x800)
    if (CURRENT_FB && fb_rotated[current_fb_index] && ppa_srm_handle && panel_handle) {
        bool partial_rotation = (row_y >= 0 && row_y < FB_HEIGHT && row_height > 0);
        
        if (partial_rotation) {
            // Partial rotation: only rotate the changed row region (much faster)
            // After 270° CCW rotation: 
            // - Row at y becomes column at x = (FB_HEIGHT - 1 - y)
            // - Row at y+row_height-1 becomes column at x = (FB_HEIGHT - 1 - (y + row_height - 1))
            // Since we're rotating row_height pixels, we get row_height columns in the rotated buffer
            // The columns are in reverse order: top row becomes rightmost column
            int rotated_col_right = FB_HEIGHT - 1 - row_y;  // Rightmost column (top of source region)
            int rotated_col_left = FB_HEIGHT - 1 - (row_y + row_height - 1);  // Leftmost column (bottom of source region)
            
            // Rotate only this row region
            ppa_srm_oper_config_t srm_config = {
                .in.buffer = CURRENT_FB,
                .in.pic_w = FB_WIDTH,      // 800
                .in.pic_h = FB_HEIGHT,     // 480
                .in.block_w = FB_WIDTH,    // Full width of the row
                .in.block_h = row_height,  // Height of the row region
                .in.block_offset_x = 0,
                .in.block_offset_y = row_y,  // Source row in logical framebuffer
                .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                .out.buffer = fb_rotated,
                .out.buffer_size = 480 * 800 * sizeof(uint16_t),  // Rotated size
                .out.pic_w = 480,  // After 270° rotation: height becomes width
                .out.pic_h = 800,  // After 270° rotation: width becomes height
                .out.block_offset_x = rotated_col_left,  // Leftmost column position in rotated framebuffer
                .out.block_offset_y = 0,  // Start from top
                .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,  // 270° CCW rotation
                .scale_x = 1,
                .scale_y = 1,
                .rgb_swap = 0,
                .byte_swap = 0,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            
            // Perform partial rotation
            esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
            if (ret == ESP_OK) {
                // Extract rotated columns to temporary buffer (columns are interleaved in rotated buffer)
                // For esp_lcd_panel_draw_bitmap, we need contiguous row-major data
                // The panel expects data in left-to-right order, so we extract columns from left to right
                static uint16_t col_buffer[800 * 20];  // Static buffer for up to 20 rows (should be enough)
                if (row_height <= 20) {
                // Extract each column from left to right and pack them row-major for the panel update
                // Optimized: use row-major access pattern for better cache locality
                // Access rotated buffer column-wise but pack row-major in col_buffer
                for (int col = 0; col < row_height; col++) {
                    int rotated_col_x = rotated_col_left + col;  // Extract from left to right
                    uint16_t *col_buf_ptr = col_buffer + col * 800;  // Row start in destination
                    // Access rotated buffer column (stride=480) - cache-friendly if we process multiple columns
                    for (int i = 0; i < 800; i++) {
                        col_buf_ptr[i] = fb_rotated[i * 480 + rotated_col_x];
                    }
                }
                    // Send the rotated column region to panel (partial update)
                    // x_start = rotated_col_left, x_end = rotated_col_right + 1 (exclusive)
                    // Width is row_height columns, height is 800 pixels
                    esp_lcd_panel_draw_bitmap(panel_handle, rotated_col_left, 0, rotated_col_right + 1, 800, col_buffer);
                } else {
                    // Fallback to full rotation if row_height is too large
                    ESP_LOGW(TAG, "Row height %d too large for partial rotation, using full rotation", row_height);
                    partial_rotation = false;
                }
            } else {
                ESP_LOGE(TAG, "PPA partial rotation failed: %s", esp_err_to_name(ret));
                partial_rotation = false;
            }
        }
        
        if (!partial_rotation) {
            // Full screen rotation (first render or fallback)
            ppa_srm_oper_config_t srm_config = {
                .in.buffer = CURRENT_FB,
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
}
#endif

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
    ppa_srm_handle = hw_accel_get_srm_handle();
    ppa_fill_handle = hw_accel_get_fill_handle();

    // Allocate single logical landscape framebuffer (800x480) from DMA-capable PSRAM
    // Use 64-byte alignment for L2 cache line optimization (ESP32-P4 has 64-byte cache lines)
    size_t fb_size = FB_WIDTH * FB_HEIGHT * sizeof(uint16_t);  // 800x480x2 = 768000 bytes
    fb = (uint16_t*)heap_caps_aligned_alloc(64, fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (fb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate logical framebuffer from DMA-capable PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Allocated logical framebuffer: %zu bytes (800x480)", fb_size);
    
    // Allocate single rotated framebuffer (480x800) for PPA output
    // Use 64-byte alignment for L2 cache line optimization
    size_t fb_rotated_size = 480 * 800 * sizeof(uint16_t);  // 480x800x2 = 768000 bytes
    fb_rotated = (uint16_t*)heap_caps_aligned_alloc(64, fb_rotated_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (fb_rotated == NULL) {
        ESP_LOGE(TAG, "Failed to allocate rotated framebuffer from DMA-capable PSRAM");
        return;
    }
    ESP_LOGI(TAG, "Allocated rotated framebuffer: %zu bytes (480x800)", fb_rotated_size);
    
    // Initialize framebuffer to black
    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    ESP_LOGI(TAG, "Initialized framebuffer to black");

    // Get input event queue from BSP
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

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
    static file_browser_t browser = {0};
    static bool browser_active = false;
    static uint8_t *mod_file_data = NULL;
    static size_t mod_file_size = 0;
    static char current_mod_path[MAX_FILENAME_LEN] = {0};  // Store current MOD file path for display

    // Start with file browser if SD card is mounted
    // NOTE: Don't call blit() here - the main loop will render and call blit() once per frame
    if (sdcard_is_mounted()) {
        browser_active = true;
        file_browser_init(&browser, "/sdcard");
        // Initial file browser will be drawn in main loop when browser_rendered is false
    } else {
        // No SD card - show error (will be drawn in main loop)
        browser_active = false;  // Don't show browser if no SD card
    }

    // Tracker UI state - center-based scrolling view with one line per pattern row
    #define MAX_TRACKER_ROWS 60  // Maximum number of row lines to store (more than screen can display for scrolling)
    struct tracker_tick {
        struct xmp_channel_info channels[MOD_MAX_CHANNELS];
        int pos;
        int pattern;
        int row;
        int num_channels;
    };
    static struct tracker_tick tick_history[MAX_TRACKER_ROWS] = {0};  // Rows above center (already played)
    static int tick_history_count = 0;
    static struct tracker_tick current_tick = {0};  // Current row at center (being played)
    static bool current_tick_valid = false;  // True if current_tick contains valid data
    static int last_row = -1;
    static struct xmp_module_info mod_info = {0};
    static bool mod_info_loaded = false;
    
    // Smooth scrolling state - center-based scrolling
    static int smooth_scroll_offset = 0;  // Current scroll offset in pixels (0 to line_height) - rows scroll into center from below
    static bool pending_new_row = false;  // True when a new row is waiting to be added (below center)
    static struct tracker_tick pending_tick = {0};  // New row data waiting to scroll into center
    
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

    while (1) {
        PROFILING_START(frame);
        
        // Track if we need to render this frame
        static bool needs_render = false;
        bool did_render = false;  // Track if we actually rendered anything this frame
        
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

                                            // Reject files larger than 10MB
                                            if (file_size > MAX_MOD_FILE_SIZE) {
                                                fclose(f);
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "File too large (>10MB)");
                                                blit(-1, 0);
                                                break;
                                            }

                                            // Free previous MOD data if any
                                            if (mod_file_data) {
                                                free(mod_file_data);
                                            }

                                            // Allocate file buffer in PSRAM for large tracker files
                                            mod_file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                                            if (mod_file_data) {
                                                size_t read = fread(mod_file_data, 1, file_size, f);
                                                fclose(f);

                                                if (read == file_size) {
                                                    mod_file_size = file_size;
                                                    // Store file path for display
                                                    strncpy(current_mod_path, selected_path, sizeof(current_mod_path) - 1);
                                                    current_mod_path[sizeof(current_mod_path) - 1] = '\0';
                                                    // Show loading message before blocking load operation
                                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                                          (FB_WIDTH - 10 * FONT_WIDTH * 2) / 2,  // Center horizontally (10 chars * 8px * 2 scale)
                                                                          (FB_HEIGHT - FONT_HEIGHT * 2) / 2,    // Center vertically (16px * 2 scale)
                                                                          RGB565_WHITE, 2, "Loading...");
                                                    blit(-1, 0);
                                                    res = mod_player_load(mod_file_data, mod_file_size);
                                                    if (res == ESP_OK) {
                                                        res = mod_player_start();
                                                    if (res == ESP_OK) {
                                                        browser_active = false;
                                                        mod_info_loaded = false;  // Force reload of module info
                                                        current_view = VIEW_TRACKER;  // Reset to tracker view
                                                        // Reset VU meter levels for new module
                                                        for (int i = 0; i < MOD_MAX_CHANNELS; i++) {
                                                            channel_vu_levels[i] = 0.0f;
                                                            channel_vu_peaks[i] = 0.0f;
                                                            channel_peak_hold[i] = 0;
                                                        }
                                                        // Clear screen immediately to prevent white flash
                                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                                                        // Tracker UI will be drawn in main loop
                                                    } else {
                                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                            blit(-1, 0);
                                                        }
                                                    } else {
                                                        free(mod_file_data);
                                                        mod_file_data = NULL;
                                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                        blit(-1, 0);
                                                    }
                                                } else {
                                                    free(mod_file_data);
                                                    mod_file_data = NULL;
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
                                        } else {
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                        blit(-1, 0);
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
                        
                        // Mark browser for redraw (will be rendered after input processing)
                        if (browser_needs_redraw) {
                            needs_render = true;
                        }
                        } else if (mod_player_is_playing()) {
                            // Volume control during playback
                            // Actual range: 20%-100%, displayed as 0%-100%
                            // Step: 5% displayed = 4% actual (0.05 * 0.80)
                            float current_vol = 0.0f;
                            if (audio_get_volume(&current_vol) == ESP_OK) {
                                float new_vol = current_vol;
                                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
                                    new_vol += 0.04f;  // 5% display step
                                    if (new_vol > 1.00f) new_vol = 1.00f;  // Cap at 100%
                                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                                    new_vol -= 0.04f;  // 5% display step
                                    if (new_vol < 0.20f) new_vol = 0.20f;  // Cap at 20%
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
                            // Mark browser for redraw (will be rendered after input processing)
                            needs_render = true;
                        }
                    }
                    
                    // Handle F4 key (0x3E) for channel page switching during playback
                    if (!browser_active && mod_player_is_playing() && sc == 0x3E) {
                        tab_pressed_this_frame = true;  // Flag will be checked during rendering
                    }

                    // Handle F3 key (0x3D) for view mode toggle during playback
                    if (!browser_active && mod_player_is_playing() && sc == 0x3D) {
                        view_key_pressed = true;  // Flag will be checked during rendering
                    }

                    // Handle Space bar (0x39) for pause/resume during playback
                    if (!browser_active && mod_player_is_playing() && sc == 0x39) {
                        if (mod_player_is_paused()) {
                            mod_player_resume();
                        } else {
                            mod_player_pause();
                        }
                    }

                    // Handle number keys (0-9) for channel mute toggle during playback
                    // Standard PC scancodes (Set 1): 0=0x0B, 1-9=0x02-0x0A
                    // Direct mapping: key number = channel number
                    if (!browser_active && mod_player_is_playing()) {
                        int channel = -1;
                        if (sc >= 0x02 && sc <= 0x0A) {
                            // Keys 1-9: scancodes 0x02-0x0A map directly to channels 1-9
                            channel = sc - 0x01;
                        } else if (sc == 0x0B) {
                            // Key 0: scancode 0x0B maps to channel 0
                            channel = 0;
                        }
                        
                        if (channel >= 0 && channel < MOD_MAX_CHANNELS) {
                            // Toggle channel mute
                            esp_err_t mute_res = mod_player_toggle_channel_mute(channel);
                            if (mute_res == ESP_OK) {
                                // Force redraw on next frame to show color change
                                // The mute state will be checked during rendering
                            }
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

                                        // Reject files larger than 10MB
                                        if (file_size > MAX_MOD_FILE_SIZE) {
                                            fclose(f);
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "File too large (>10MB)");
                                            blit(-1, 0);
                                            break;
                                        }

                                        if (mod_file_data) {
                                            free(mod_file_data);
                                        }

                                        // Allocate file buffer in PSRAM for large tracker files
                                        mod_file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                                        if (mod_file_data) {
                                            size_t read = fread(mod_file_data, 1, file_size, f);
                                            fclose(f);

                                            if (read == file_size) {
                                                mod_file_size = file_size;
                                                // Store file path for display
                                                strncpy(current_mod_path, selected_path, sizeof(current_mod_path) - 1);
                                                current_mod_path[sizeof(current_mod_path) - 1] = '\0';
                                                // Show loading message before blocking load operation
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                                      (FB_WIDTH - 10 * FONT_WIDTH * 2) / 2,  // Center horizontally (10 chars * 8px * 2 scale)
                                                                      (FB_HEIGHT - FONT_HEIGHT * 2) / 2,    // Center vertically (16px * 2 scale)
                                                                      RGB565_WHITE, 2, "Loading...");
                                                blit(-1, 0);
                                                res = mod_player_load(mod_file_data, mod_file_size);
                                                if (res == ESP_OK) {
                                                    res = mod_player_start();
                                                    if (res == ESP_OK) {
                                                        browser_active = false;
                                                        mod_info_loaded = false;  // Force reload of module info
                                                        current_view = VIEW_TRACKER;  // Reset to tracker view
                                                        // Reset VU meter levels for new module
                                                        for (int i = 0; i < MOD_MAX_CHANNELS; i++) {
                                                            channel_vu_levels[i] = 0.0f;
                                                            channel_vu_peaks[i] = 0.0f;
                                                            channel_peak_hold[i] = 0;
                                                        }
                                                        // Clear screen immediately to prevent white flash
                                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                                                        // Tracker UI will be drawn in main loop
                                                        browser_needs_redraw = false;  // Don't redraw browser, we're playing now
                                                    } else {
                                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                        blit(-1, 0);
                                                    }
                                                } else {
                                                    free(mod_file_data);
                                                    mod_file_data = NULL;
                                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                    blit(-1, 0);
                                                }
                                            } else {
                                                free(mod_file_data);
                                                mod_file_data = NULL;
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            fclose(f);
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                            blit(-1, 0);
                                        }
                                    } else {
                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                                        blit(-1, 0);
                                    }
                                }
                            } else if (select_res == ESP_OK && is_directory) {
                                // Directory entered - browser already refreshed by file_browser_select()
                                browser_needs_redraw = true;
                            }
                        }
                        
                        // Mark browser for redraw (will be rendered after input processing)
                        if (browser_needs_redraw) {
                            needs_render = true;
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

                                    // Reject files larger than 10MB
                                    if (file_size > MAX_MOD_FILE_SIZE) {
                                        fclose(f);
                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "File too large (>10MB)");
                                        blit(-1, 0);
                                        break;
                                    }

                                    if (mod_file_data) {
                                        free(mod_file_data);
                                    }

                                    // Allocate file buffer in PSRAM for large tracker files
                                    mod_file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                                    if (mod_file_data) {
                                        size_t read = fread(mod_file_data, 1, file_size, f);
                                        fclose(f);

                                        if (read == file_size) {
                                            mod_file_size = file_size;
                                            // Store file path for display
                                            strncpy(current_mod_path, selected_path, sizeof(current_mod_path) - 1);
                                            current_mod_path[sizeof(current_mod_path) - 1] = '\0';
                                            // Show loading message before blocking load operation
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                                  (FB_WIDTH - 10 * FONT_WIDTH * 2) / 2,  // Center horizontally (10 chars * 8px * 2 scale)
                                                                  (FB_HEIGHT - FONT_HEIGHT * 2) / 2,    // Center vertically (16px * 2 scale)
                                                                  RGB565_WHITE, 2, "Loading...");
                                            blit(-1, 0);
                                            res = mod_player_load(mod_file_data, mod_file_size);
                                            if (res == ESP_OK) {
                                                res = mod_player_start();
                                                    if (res == ESP_OK) {
                                                        browser_active = false;
                                                        mod_info_loaded = false;  // Force reload of module info
                                                        current_view = VIEW_TRACKER;  // Reset to tracker view
                                                        // Reset VU meter levels for new module
                                                        for (int i = 0; i < MOD_MAX_CHANNELS; i++) {
                                                            channel_vu_levels[i] = 0.0f;
                                                            channel_vu_peaks[i] = 0.0f;
                                                            channel_peak_hold[i] = 0;
                                                        }
                                                        // Clear screen immediately to prevent white flash
                                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                                                        // Tracker UI will be drawn in main loop
                                                    } else {
                                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                    blit(-1, 0);
                                                }
                                            } else {
                                                free(mod_file_data);
                                                mod_file_data = NULL;
                                                ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            free(mod_file_data);
                                            mod_file_data = NULL;
                                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                            blit(-1, 0);
                                        }
                                    } else {
                                        fclose(f);
                                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                        blit(-1, 0);
                                    }
                                } else {
                                    ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                    font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                                    blit(-1, 0);
                                }
                            }
                        } else if (select_res == ESP_OK && is_directory) {
                            // Directory entered - browser already refreshed by file_browser_select()
                            browser_needs_redraw = true;
                        }
                        
                        // Mark browser for redraw (will be rendered after input processing)
                        if (browser_needs_redraw) {
                            needs_render = true;
                        }
                    }
                    
                    // Debug: Scancode event (commented out)
                    break;
                }
                default:
                    break;
            }
        }  // End of while loop processing all pending input events
        
        // Render file browser if needed (input handlers set needs_render flag)
        // Also render on first loop if browser is active but hasn't been rendered yet
        static bool browser_rendered = false;
        if (browser_active && (needs_render || !browser_rendered)) {
            draw_file_browser(&browser);
            needs_render = false;  // Clear flag after rendering
            browser_rendered = true;  // Mark as rendered
            did_render = true;  // Mark that we rendered this frame
        } else if (!browser_active) {
            browser_rendered = false;  // Reset when browser becomes inactive
        }
        
        // Render "no SD card" error if needed (only once, or if it changed)
        static bool error_rendered = false;
        static bool last_sdcard_mounted = false;
        bool current_sdcard_mounted = sdcard_is_mounted();
        if (!browser_active && !mod_player_is_playing() && !current_sdcard_mounted && (!error_rendered || last_sdcard_mounted != current_sdcard_mounted)) {
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
        
        // Render tracker UI if playing
        // For smooth scrolling, we need to render every frame, not just on row changes
        if (mod_player_is_playing() && !browser_active) {
            // Track mode transitions - clear framebuffer when switching from browser to playback
            static bool last_browser_active = true;
            if (last_browser_active && !browser_active) {
                // Switching from browser to playback - clear framebuffer for clean transition
                ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                did_render = true;  // Ensure we blit to show the cleared screen
            }
            last_browser_active = browser_active;
            
            PROFILING_START(render);
            struct xmp_frame_info frame_info;
            if (mod_player_get_frame_info(&frame_info) == ESP_OK) {
                // Always render to support smooth scrolling (updates every frame during scroll)
                // Row change detection is handled inside the render logic
                {
                    // Load module info if not loaded
                    static bool tracker_initialized = false;  // Declare here so it persists across renders
                    if (!mod_info_loaded) {
                        mod_player_get_module_info(&mod_info);
                        mod_info_loaded = true;
                        // Reset tracker_initialized when module info is reloaded (new MOD started)
                        tracker_initialized = false;
                        tick_history_count = 0;  // Reset tick history for new MOD
                        last_row = -1;  // Reset last_row to force first render
                        channel_page_offset = 0;  // Reset to first page when loading new MOD
                    }
                    
                    int num_channels = mod_info.mod ? mod_info.mod->chn : 0;
                    if (num_channels > 0 && num_channels <= MOD_MAX_CHANNELS) {
                        // Handle view mode toggle ('V' key)
                        if (view_key_pressed) {
                            current_view = (playback_view_t)((current_view + 1) % 3);  // Cycle through VIEW_TRACKER, VIEW_SPECTRUM, VIEW_INFO
                            view_key_pressed = false;
                            // Clear framebuffer when switching views
                            ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_BG_PRIMARY);
                        }

                        // Update VU meter decay for smooth animation
                        for (int ch = 0; ch < num_channels; ch++) {
                            // Get current volume from frame info
                            float target_level = (float)frame_info.channel_info[ch].volume / 64.0f;

                            // Smoothed attack and decay to reduce flicker
                            if (target_level > channel_vu_levels[ch]) {
                                // Smoothed attack - lerp toward target
                                channel_vu_levels[ch] += (target_level - channel_vu_levels[ch]) * VU_ATTACK_RATE;
                            } else {
                                // Slow decay
                                channel_vu_levels[ch] *= (1.0f - VU_DECAY_RATE);
                                if (channel_vu_levels[ch] < 0.01f) {
                                    channel_vu_levels[ch] = 0.0f;
                                }
                            }

                            // Peak hold and decay
                            if (target_level > channel_vu_peaks[ch]) {
                                channel_vu_peaks[ch] = target_level;
                                channel_peak_hold[ch] = VU_PEAK_HOLD_FRAMES;
                            } else if (channel_peak_hold[ch] > 0) {
                                channel_peak_hold[ch]--;
                            } else {
                                channel_vu_peaks[ch] *= (1.0f - VU_PEAK_DECAY_RATE);
                                if (channel_vu_peaks[ch] < 0.01f) {
                                    channel_vu_peaks[ch] = 0.0f;
                                }
                            }
                        }

                        // Channel pagination: Show 4 channels per page, switch pages with Tab
                        // Check for Tab key press flag (set by scancode handler)
                        if (tab_pressed_this_frame) {
                            // Cycle to next page (4 channels per page)
                            int max_pages = (num_channels + 3) / 4;  // Round up
                            channel_page_offset = (channel_page_offset + 4) % (max_pages * 4);
                            if (channel_page_offset >= num_channels) {
                                channel_page_offset = 0;  // Wrap around
                            }
                            tab_pressed_this_frame = false;  // Reset flag
                        }
                        
                        // Calculate which channels to display (up to 4 channels per page)
                        int channels_per_page = 4;
                        int start_channel = channel_page_offset;
                        int end_channel = start_channel + channels_per_page;
                        if (end_channel > num_channels) {
                            end_channel = num_channels;
                        }
                        int visible_channels = end_channel - start_channel;

                        // Update global VU meter layout cache
                        vu_start_channel = start_channel;
                        vu_visible_channels = visible_channels;

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
                        vu_cached_line_height = line_height;

                        // Header height: use smaller header to maximize tracker content area
                        // Font scale 1 = 16px text height, add small padding
                        int header_height = FONT_HEIGHT + 4;  // 20px total
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
                        
                        void draw_header(void) {
                            // Clear header area with theme background
                            ui_draw_vgradient(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                              0, MARGIN_TOP, FB_WIDTH, header_height,
                                              THEME_BG_HEADER, THEME_BG_PRIMARY);

                            // Use module metadata name instead of filename
                            const char *mod_name = (mod_info.mod && mod_info.mod->name[0])
                                                   ? mod_info.mod->name : "Unknown";

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
                            draw_header();
                            
                            // Initialize current tick with first frame data
                            memcpy(current_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                            current_tick.pos = frame_info.pos;
                            current_tick.pattern = frame_info.pattern;
                            current_tick.row = frame_info.row;
                            current_tick.num_channels = num_channels;
                            current_tick_valid = true;
                            
                            // Draw current row at center with highlight background
                            int y = center_y;
                            
                            // Draw highlight background for current row (full width of content area)
                            int highlight_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                            ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, y, highlight_width, line_height, RGB565_DARK_BLUE);
                            
                            // Draw current frame data at center (from current_tick) - with pagination and centering
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
                            // Initialize last_row to current row to prevent immediate scrolling on first frame
                            last_row = current_tick.row;
                            // Reset smooth scrolling state for clean start
                            smooth_scroll_offset = 0;
                            pending_new_row = false;
                            // Full screen blit on first render
                            blit(-1, 0);  // -1 means full screen, row_height ignored
                        } else {
                            // Subsequent renders - smooth scrolling
                            // NOTE: Rows are NOT added to history here - they're added when scroll completes
                            // This prevents duplicate rows from being added every frame
                            
                            // Always redraw header every frame to prevent smudging from scrolling rows
                            // This ensures the header area stays clean as rows scroll underneath
                            draw_header();
                            
                            // Pattern-timed smooth scrolling: scroll one line_height over the duration of one pattern row
                            // This keeps scrolling in sync with pattern playback
                            // Reserve line_height at bottom for VU meters
                            int scrollable_height = FB_HEIGHT - header_y - MARGIN_BOTTOM - line_height;
                            
                            // Track when current row started and its duration
                            static TickType_t row_start_tick = 0;
                            static float current_row_duration_ms = 0.0f;
                            
                            // Fixed grid approach: update history immediately when row changes
                            // This keeps history and future rows in sync (both update based on current position)
                            if (frame_info.row != last_row) {
                                // Immediately add current row to history when it changes (for fixed grid)
                                // This ensures history rows appear at the same rate as future rows disappear
                                if (current_tick_valid && last_row >= 0) {
                                    // Move previous row to history immediately
                                    if (tick_history_count < MAX_TRACKER_ROWS) {
                                        memcpy(tick_history[tick_history_count].channels, current_tick.channels, sizeof(current_tick.channels));
                                        tick_history[tick_history_count].pos = current_tick.pos;
                                        tick_history[tick_history_count].pattern = current_tick.pattern;
                                        tick_history[tick_history_count].row = current_tick.row;
                                        tick_history[tick_history_count].num_channels = current_tick.num_channels;
                                        tick_history_count++;
                                    } else {
                                        // Shift history (oldest first, so remove oldest)
                                        memmove(tick_history, tick_history + 1, (MAX_TRACKER_ROWS - 1) * sizeof(struct tracker_tick));
                                        memcpy(tick_history[MAX_TRACKER_ROWS - 1].channels, current_tick.channels, sizeof(current_tick.channels));
                                        tick_history[MAX_TRACKER_ROWS - 1].pos = current_tick.pos;
                                        tick_history[MAX_TRACKER_ROWS - 1].pattern = current_tick.pattern;
                                        tick_history[MAX_TRACKER_ROWS - 1].row = current_tick.row;
                                        tick_history[MAX_TRACKER_ROWS - 1].num_channels = current_tick.num_channels;
                                    }
                                }
                                // Update current tick with new row data immediately
                                memcpy(current_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                                current_tick.pos = frame_info.pos;
                                current_tick.pattern = frame_info.pattern;
                                current_tick.row = frame_info.row;
                                current_tick.num_channels = num_channels;
                                current_tick_valid = true;
                                
                                last_row = frame_info.row;
                            }
                            
                            // Check if we have a new row - allow continuous scrolling without waiting
                            if (frame_info.row != last_row && !pending_new_row) {
                                // New row detected - prepare it for smooth scrolling
                                pending_new_row = true;
                                memcpy(pending_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                                pending_tick.pos = frame_info.pos;
                                pending_tick.pattern = frame_info.pattern;
                                pending_tick.row = frame_info.row;
                                pending_tick.num_channels = num_channels;
                                
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
                                if (smooth_scroll_offset >= line_height) {
                                    smooth_scroll_offset = 0;  // Reset if previous row just completed
                                }
                                // Otherwise keep current offset for seamless continuous scrolling
                                
                                // Debug: log row timing info (only occasionally to avoid spam)
                                static int debug_row_count = 0;
                                if (debug_row_count++ % 20 == 0) {
                                    ESP_LOGI(TAG, "Row timing: speed=%d, frame_time=%d us, single_row=%.1f ms, scroll_duration=%.1f ms (over %d rows), line_height=%d", 
                                            frame_info.speed, frame_info.frame_time, single_row_duration_ms, 
                                            current_row_duration_ms, ROWS_PER_SCROLL, line_height);
                                }
                                
                                last_row = frame_info.row;  // Update to prevent repeated detection
                            }
                            
                            // Calculate scroll position based on elapsed time since row started
                            // Only scroll if we have a pending row
                            if (pending_new_row && current_row_duration_ms > 0.0f) {
                                TickType_t current_time = xTaskGetTickCount();
                                TickType_t elapsed_ticks = current_time - row_start_tick;
                                float elapsed_ms = (float)elapsed_ticks * (1000.0f / configTICK_RATE_HZ);
                                
                                // Calculate scroll position: 0 to line_height over row_duration_ms
                                // This ensures we scroll exactly one line_height over the duration of one pattern row
                                float scroll_progress = elapsed_ms / current_row_duration_ms;
                                if (scroll_progress > 1.0f) scroll_progress = 1.0f;  // Clamp to 1.0
                                
                                smooth_scroll_offset = (int)(scroll_progress * (float)line_height);
                                
                                // Note: In fixed grid mode, history is updated immediately when frame_info.row changes
                                // (handled above), so we don't need to move rows here. The smooth_scroll_offset
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
                                struct tracker_tick *tick = NULL;
                                bool is_current_row = false;
                                
                                if (row_idx < center_row_idx) {
                                    // Rows above center: history rows (most recent closest to center)
                                    int history_offset = center_row_idx - row_idx - 1;  // 0 = most recent, 1 = older, etc.
                                    if (history_offset < tick_history_count) {
                                        int history_idx = tick_history_count - 1 - history_offset;  // Most recent is last in history
                                        if (history_idx >= 0) {
                                            tick = &tick_history[history_idx];
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
                                                vu_channel_x[i] = cache_x;
                                                vu_channel_width[i] = channel_widths[i];
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
                    if (current_view == VIEW_TRACKER) {
                        // Layout: VU meters above hint bar at bottom
                        int hint_bar_height = 20;  // Height for hint bar
                        int vu_height = vu_cached_line_height;
                        int hint_bar_y = FB_HEIGHT - MARGIN_BOTTOM - hint_bar_height;
                        int vu_footer_y = hint_bar_y - vu_height;

                        // Draw background for VU area
                        ppa_fill_rect(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                      0, vu_footer_y, FB_WIDTH, vu_height, THEME_BG_SECONDARY);

                        // Draw VU meters aligned with channel columns (using cached positions)
                        int visible_ch = vu_visible_channels;
                        if (visible_ch < 1) visible_ch = 4;
                        for (int i = 0; i < visible_ch && i < 4; i++) {
                            int ch = vu_start_channel + i;
                            if (ch >= num_channels || ch >= MOD_MAX_CHANNELS) break;

                            // Use cached column positions for alignment
                            int vu_x = vu_channel_x[i];
                            int vu_width = vu_channel_width[i];
                            if (vu_width < 10) vu_width = 50;  // Fallback if not cached yet

                            ui_draw_vu_meter(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                             vu_x, vu_footer_y + 2, vu_width, vu_height - 4,
                                             channel_vu_levels[ch], channel_vu_peaks[ch],
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
                    } else if (current_view == VIEW_SPECTRUM) {
                        // Full-screen spectrum analyzer view
                        ppa_fill_framebuffer(CURRENT_FB, FB_WIDTH, FB_HEIGHT, THEME_SPECTRUM_BG);

                        // Draw header with song info
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, MARGIN_TOP,
                                                THEME_TEXT_PRIMARY, 2, "SPECTRUM ANALYZER");

                        // Draw spectrum bars
                        uint8_t bands[SPECTRUM_NUM_BANDS] = {0};
                        uint8_t peaks[SPECTRUM_NUM_BANDS] = {0};
                        if (spectrum) {
                            spectrum_get_bands_and_peaks(spectrum, bands, peaks, SPECTRUM_NUM_BANDS);
                            spectrum_update_decay(spectrum);
                        } else {
                            // No spectrum analyzer - show simulated bars from channel volumes
                            for (int i = 0; i < SPECTRUM_NUM_BANDS && i < num_channels; i++) {
                                bands[i] = (uint8_t)(channel_vu_levels[i] * 255.0f);
                                peaks[i] = (uint8_t)(channel_vu_peaks[i] * 255.0f);
                            }
                        }

                        int hint_bar_height_spec = 20;
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
                    } else if (current_view == VIEW_INFO) {
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
                        const char *mod_name = mod_info.mod ? mod_info.mod->name : "Unknown";
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
                        int pat_count = mod_info.mod ? mod_info.mod->pat : 0;
                        snprintf(info_line, sizeof(info_line), "Patterns: %d", pat_count);
                        font_draw_string_scaled(CURRENT_FB, FB_WIDTH, FB_HEIGHT,
                                                MARGIN_LEFT, info_y, THEME_TEXT_SECONDARY, 2, info_line);
                        info_y += line_spacing;

                        // Current position
                        int song_len = mod_info.mod ? mod_info.mod->len : 0;
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
                                           channel_vu_levels[ch], THEME_VU_BG);
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

                    PROFILING_END(render, render);
                    // Note: last_row is now updated when we finish scrolling, not immediately
                    // This allows smooth scrolling to complete before marking the row as processed
                    did_render = true;  // Tracker UI always renders (smooth scrolling)
                }
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
