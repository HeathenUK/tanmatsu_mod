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
#include "hal/color_types.h"  // For color_pixel_argb8888_data_t
#include "esp_attr.h"  // For IRAM_ATTR
#include "esp_async_memcpy.h"  // For GDMA memory operations
#include "wifi_connection.h"
#include "wifi_remote.h"
#include "profiling.h"

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
static ppa_client_handle_t ppa_fill_handle = NULL;  // PPA Fill client for hardware-accelerated fills
static async_memcpy_handle_t async_memcpy_handle = NULL;  // GDMA async memcpy handle
static QueueHandle_t input_event_queue = NULL;

// RGB565 to ARGB8888 conversion helper for PPA Fill
// RGB565: 0bRRRRRGGGGGGBBBBB
// ARGB8888: 0xAARRGGBB (A=0xFF for opaque)
static inline color_pixel_argb8888_data_t rgb565_to_argb8888(uint16_t rgb565) {
    color_pixel_argb8888_data_t argb;
    // Extract RGB components from RGB565
    uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits (scale by 8)
    uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits (scale by 4)
    uint8_t b = (rgb565 & 0x1F) << 3;          // 5 bits -> 8 bits (scale by 8)
    // Set ARGB8888 (A=0xFF for opaque)
    argb.a = 0xFF;
    argb.r = r;
    argb.g = g;
    argb.b = b;
    return argb;
}

// Hardware-accelerated framebuffer fill using PPA Fill
// Falls back to CPU fill if PPA is unavailable or operation fails
static void IRAM_ATTR ppa_fill_framebuffer(uint16_t *fb, int width, int height, uint16_t color) {
    if (!fb || width <= 0 || height <= 0) {
        return;
    }
    
    // Try PPA Fill first (hardware-accelerated)
    if (ppa_fill_handle && fb) {
        // Convert RGB565 to ARGB8888 for PPA
        color_pixel_argb8888_data_t fill_color = rgb565_to_argb8888(color);
        
        // Configure PPA Fill operation
        ppa_fill_oper_config_t fill_config = {
            .out = {
                .buffer = fb,
                .buffer_size = width * height * sizeof(uint16_t),
                .pic_w = width,
                .pic_h = height,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .fill_cm = PPA_FILL_COLOR_MODE_RGB565,  // Output format is RGB565
            },
            .fill_block_w = width,
            .fill_block_h = height,
            .fill_argb_color = fill_color,
            .mode = PPA_TRANS_MODE_BLOCKING,
            .user_data = NULL,
        };
        
        esp_err_t ret = ppa_do_fill(ppa_fill_handle, &fill_config);
        if (ret == ESP_OK) {
            return;  // Success - hardware fill completed
        }
        // Log fallback to CPU fill
        ESP_LOGW(TAG, "PPA Fill failed (%s), falling back to CPU fill", esp_err_to_name(ret));
    } else {
        // PPA Fill handle not available
        ESP_LOGD(TAG, "PPA Fill handle not available, using CPU fill");
    }
    
    // CPU fallback (original fb_fill implementation)
    int total_pixels = width * height;
    for (int i = 0; i < total_pixels; i++) {
        fb[i] = color;
    }
}

// Hardware-accelerated rectangle fill using PPA Fill
// Falls back to CPU fill if PPA is unavailable or operation fails
static void IRAM_ATTR ppa_fill_rect(uint16_t *fb, int width, int height, int x, int y, int w, int h, uint16_t color) {
    if (!fb || width <= 0 || height <= 0 || w <= 0 || h <= 0) {
        return;
    }
    
    // Clamp rectangle to framebuffer bounds
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > width) {
        w = width - x;
    }
    if (y + h > height) {
        h = height - y;
    }
    if (w <= 0 || h <= 0) {
        return;  // Rectangle is completely outside framebuffer
    }
    
    // Try PPA Fill first (hardware-accelerated)
    if (ppa_fill_handle) {
        // Convert RGB565 to ARGB8888 for PPA
        color_pixel_argb8888_data_t fill_color = rgb565_to_argb8888(color);
        
        // Configure PPA Fill operation for the rectangle region
        ppa_fill_oper_config_t fill_config = {
            .out = {
                .buffer = fb,
                .buffer_size = width * height * sizeof(uint16_t),
                .pic_w = width,
                .pic_h = height,
                .block_offset_x = x,  // Rectangle X position
                .block_offset_y = y,  // Rectangle Y position
                .fill_cm = PPA_FILL_COLOR_MODE_RGB565,  // Output format is RGB565
            },
            .fill_block_w = w,  // Rectangle width
            .fill_block_h = h,  // Rectangle height
            .fill_argb_color = fill_color,
            .mode = PPA_TRANS_MODE_BLOCKING,
            .user_data = NULL,
        };
        
        esp_err_t ret = ppa_do_fill(ppa_fill_handle, &fill_config);
        if (ret == ESP_OK) {
            return;  // Success - hardware fill completed
        }
        // Log fallback to CPU fill
        ESP_LOGW(TAG, "PPA Fill rect failed (%s), falling back to CPU fill", esp_err_to_name(ret));
    } else {
        // PPA Fill handle not available
        ESP_LOGD(TAG, "PPA Fill handle not available, using CPU fill for rect");
    }
    
    // CPU fallback (original fb_rect implementation)
    for (int dy = 0; dy < h; dy++) {
        int fy = y + dy;
        if (fy >= 0 && fy < height) {
            for (int dx = 0; dx < w; dx++) {
                int fx = x + dx;
                if (fx >= 0 && fx < width) {
                    fb[fy * width + fx] = color;
                }
            }
        }
    }
}

// RGB565 color constants
#define RGB565_BLACK   0x0000
#define RGB565_WHITE   0xFFFF
#define RGB565_RED     0xF800
#define RGB565_GREEN   0x07E0
#define RGB565_BLUE    0x001F
#define RGB565_YELLOW  0xFFE0
#define RGB565_MAGENTA 0xF81F
#define RGB565_CYAN    0x07FF
#define RGB565_GRAY    0x8410  // Medium gray for muted channels (50% brightness)

// KAMI support removed - Tanmatsu only

// Helper function to draw file browser view
// Helper function to format channel string consistently
// use_compact: true for compact format (C4 01 A02), false for full format (C-4 01 A02)
// Always uses leading zeros for consistency
static void IRAM_ATTR format_channel_string(char *ch_str, size_t ch_str_size, 
                                   unsigned char note, unsigned char ins, 
                                   unsigned char fxt, unsigned char fxp, 
                                   bool use_compact, const char *note_names[12]) {
    char note_str[4];
    if (note > 0 && note <= 96) {
        int note_idx = (note - 1) % 12;
        int octave = (note - 1) / 12;
        if (note_idx < 0) note_idx = 0;
        if (note_idx > 11) note_idx = 11;
        if (octave < 0) octave = 0;
        if (octave > 9) octave = 9;
        const char *note_name = note_names[note_idx];
        if (use_compact) {
            // Compact: "C4" or "C#4"
            if (note_name[1] == '#') {
                snprintf(note_str, sizeof(note_str), "%c#%d", note_name[0], octave);
            } else {
                snprintf(note_str, sizeof(note_str), "%c%d", note_name[0], octave);
            }
        } else {
            // Full: "C-4" or "C#4"
            snprintf(note_str, sizeof(note_str), "%s%d", note_name, octave);
        }
    } else {
        strcpy(note_str, use_compact ? "--" : "---");
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
    
    // Always use leading zeros for consistency: "01" not "1", "02" not "2"
    snprintf(ch_str, ch_str_size, "%s %02X %c%02X", note_str, ins, effect_char, fxp);
}

static void draw_file_browser(file_browser_t *browser) {
    const int font_scale = 2;
    const int line_height = FONT_HEIGHT * font_scale;  // 16 pixels for scale 2
    
    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    
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
            ppa_fill_rect(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, file_y, CONTENT_WIDTH, line_height, argb32_to_rgb565(0xFF0000FF));
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

// IRAM-safe callback for GDMA completion (no FreeRTOS APIs)
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
static void IRAM_ATTR scroll_framebuffer_ppa(uint16_t *fb_pixels, int src_y, int dst_y, int copy_height, int stride) {
    if (!fb_pixels || !ppa_srm_handle || copy_height <= 0) {
        return;
    }
    
    int copy_width = stride;  // Full width
    
    // Direct copy from source region to destination using PPA SRM with 0° rotation
    // Since we're scrolling up (src_y > dst_y), the regions don't overlap, so we can copy directly
    ppa_srm_oper_config_t srm_config = {
        .in.buffer = fb_pixels,  // Full framebuffer as input picture
        .in.pic_w = stride,
        .in.pic_h = FB_HEIGHT,
        .in.block_w = copy_width,
        .in.block_h = copy_height,
        .in.block_offset_x = 0,
        .in.block_offset_y = src_y,  // Start reading from src_y
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer = fb_pixels,  // Same framebuffer, different region
        .out.buffer_size = FB_HEIGHT * stride * sizeof(uint16_t),  // Full buffer size
        .out.pic_w = stride,
        .out.pic_h = FB_HEIGHT,
        .out.block_offset_x = 0,
        .out.block_offset_y = dst_y,  // Write to dst_y
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,  // No rotation, just copy
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    
    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
    if (ret != ESP_OK) {
        // Fallback to memmove if PPA fails
        ESP_LOGW(TAG, "PPA scrolling failed (%s), falling back to memmove", esp_err_to_name(ret));
        uint16_t *src_ptr = fb_pixels + src_y * stride;
        uint16_t *dst_ptr = fb_pixels + dst_y * stride;
        size_t copy_bytes = copy_height * stride * sizeof(uint16_t);
        memmove(dst_ptr, src_ptr, copy_bytes);
    }
}

// Partial rotation support: if row_y >= 0, only rotate that row region (much faster)
// For 270° CCW rotation: a horizontal row at y in logical framebuffer becomes a vertical column
// at x = (FB_HEIGHT - 1 - y) in rotated framebuffer
// row_height: height of the row region to rotate (typically line_height pixels)
void IRAM_ATTR blit(int row_y, int row_height) {
    // Use PPA to rotate logical landscape framebuffer (800x480) to ST7701S native portrait (480x800)
    if (fb && fb_rotated && ppa_srm_handle && panel_handle) {
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
                .in.buffer = fb,
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
                    for (int col = 0; col < row_height; col++) {
                        int rotated_col_x = rotated_col_left + col;  // Extract from left to right
                        for (int i = 0; i < 800; i++) {
                            col_buffer[col * 800 + i] = fb_rotated[i * 480 + rotated_col_x];
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
    
    // Initialize PPA Fill client for hardware-accelerated fills
    ppa_client_config_t ppa_fill_config = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_fill_config, &ppa_fill_handle));
    ESP_LOGI(TAG, "PPA Fill client registered for hardware-accelerated fills");
    
    // Initialize AXI-GDMA async memcpy driver for hardware-accelerated memory operations
    // AXI-GDMA supports PSRAM (unlike AHB-GDMA which only supports SRAM)
    async_memcpy_config_t async_memcpy_config = ASYNC_MEMCPY_DEFAULT_CONFIG();
    async_memcpy_config.backlog = 1;  // Only need one pending operation for scrolling
    esp_err_t ret_memcpy = esp_async_memcpy_install_gdma_axi(&async_memcpy_config, &async_memcpy_handle);
    if (ret_memcpy == ESP_OK) {
        ESP_LOGI(TAG, "AXI-GDMA async memcpy driver installed successfully (PSRAM supported)");
    } else {
        ESP_LOGW(TAG, "AXI-GDMA async memcpy driver installation failed (%s), will use CPU fallback", esp_err_to_name(ret_memcpy));
        async_memcpy_handle = NULL;  // Ensure handle is NULL on failure
    }

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
    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_BLACK, 2, "Initializing audio...");
    blit(-1, 0);
    res = audio_init();
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Audio initialized successfully");
        
        // Diagnose ES8156 codec configuration
        audio_diagnose_es8156();
        
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
        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Audio init failed");
        blit(-1, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Skip WiFi initialization for faster startup (can re-enable later if needed)
    //
    // if (wifi_remote_initialize() == ESP_OK) {
    //
    //     ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //     pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Starting WiFi stack...");
    //     blit(-1, 0);
    //     wifi_connection_init_stack();  // Start the Espressif WiFi stack
    //
    //     ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    //     pax_draw_text(&fb, BLACK, pax_font_sky_mono, 16, 0, 0, "Connecting to WiFi network...");
    //     blit(-1, 0);
    //
    //     if (wifi_connect_try_all() == ESP_OK) {
    //         ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
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
    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_BLACK, 2, "Initializing SD card...");
    blit(-1, 0);
    res = sdcard_init();
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s", esp_err_to_name(res));
        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "SD card init failed");
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 18, RGB565_BLACK, 2, "Continuing without SD");
        blit(-1, 0);
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
        blit(-1, 0);
    } else {
        // No SD card - show error
        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "SD card not mounted");
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 18, RGB565_BLACK, 2, "Insert SD card and");
        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 36, RGB565_BLACK, 2, "restart device");
    blit(-1, 0);
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
    
    // Smooth scrolling state
    static int smooth_scroll_offset = 0;  // Current scroll offset in pixels (0 to line_height)
    static bool pending_new_row = false;  // True when a new row is waiting to be added
    static struct tracker_tick pending_tick = {0};  // New row data waiting to be added
    
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
        PROFILING_START(frame);
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
                                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        // Tracker UI will be drawn in main loop
                                                    } else {
                                                    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                            blit(-1, 0);
                                                        }
                                                    } else {
                                                        free(mod_file_data);
                                                        mod_file_data = NULL;
                                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                        blit(-1, 0);
                                                    }
                                                } else {
                                                    free(mod_file_data);
                                                    mod_file_data = NULL;
                                                    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                                    blit(-1, 0);
                                                }
                                            } else {
                                                fclose(f);
                                                ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
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
                        
                        // Redraw file browser if selection changed
                        if (browser_needs_redraw) {
                            draw_file_browser(&browser);
                            blit(-1, 0);
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
                            blit(-1, 0);
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
                        
                        if (channel >= 0 && channel < XMP_MAX_CHANNELS) {
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
                                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        // Tracker UI will be drawn in main loop
                                                        browser_needs_redraw = false;  // Don't redraw browser, we're playing now
                                                    } else {
                                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                        blit(-1, 0);
                                                    }
                                                } else {
                                                    free(mod_file_data);
                                                    mod_file_data = NULL;
                                                    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                    blit(-1, 0);
                                                }
                                            } else {
                                                free(mod_file_data);
                                                mod_file_data = NULL;
                                                ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            fclose(f);
                                            ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                            blit(-1, 0);
                                        }
                                    } else {
                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                                        blit(-1, 0);
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
                            blit(-1, 0);
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
                                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                        // Tracker UI will be drawn in main loop
                                                    } else {
                                                    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to start MOD");
                                                    blit(-1, 0);
                                                }
                                            } else {
                                                free(mod_file_data);
                                                mod_file_data = NULL;
                                                ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to load MOD");
                                                blit(-1, 0);
                                            }
                                        } else {
                                            free(mod_file_data);
                                            mod_file_data = NULL;
                                            ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to read file");
                                            blit(-1, 0);
                                        }
                                    } else {
                                        fclose(f);
                                        ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Out of memory");
                                        blit(-1, 0);
                                    }
                                } else {
                                    ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                                    font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, 0, 0, RGB565_RED, 2, "Failed to open file");
                                    blit(-1, 0);
                                }
                            }
                        } else if (select_res == ESP_OK && is_directory) {
                            // Directory entered - browser already refreshed by file_browser_select()
                            browser_needs_redraw = true;
                        }
                        
                        // Redraw file browser if directory was entered
                        if (browser_needs_redraw) {
                            draw_file_browser(&browser);
                            blit(-1, 0);
                        }
                    }
                    
                    // Debug: Scancode event (commented out)
                    break;
                }
                default:
                    break;
            }
        }  // End of while loop processing all pending input events
        
        // Render tracker UI if playing
        // For smooth scrolling, we need to render every frame, not just on row changes
        if (mod_player_is_playing() && !browser_active) {
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
                            int estimated_line_height = 10 + (ch_width_estimate - 40) * 8 / 200;
                            if (estimated_line_height < 10) estimated_line_height = 10;
                            if (estimated_line_height > 18) estimated_line_height = 18;
                            
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
                            cached_num_channels = num_channels;
                        }
                        
                        int line_height = cached_line_height;
                        int max_rows_on_screen = cached_max_rows;
                        int ch_width = cached_ch_width;
                        
                        // Header height should align with line_height to prevent partial rows showing
                        // Use 1 * line_height + 3 for header area (text is scaled 2x = 16px, so fits in 1 row, plus 3px extra to prevent sliver)
                        // Header starts at MARGIN_TOP and is 1 * line_height + 3 pixels tall
                        int header_height = line_height * 1 + 3;
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
                        uint16_t *fb_pixels = fb;  // Direct framebuffer access
                        const int logical_width = FB_WIDTH;  // 800
                        const int logical_height = FB_HEIGHT;  // 480
                        int stride = logical_width;  // Pixels per row in landscape framebuffer (800)
                        
                        // Helper function to draw header (defined here so it can use variables from outer scope)
                        void draw_header(void) {
                            // Always clear header area exactly (from MARGIN_TOP to header_y, which is MARGIN_TOP + header_height)
                            // This ensures no sliver of content shows below the header and prevents smudging when updating
                            // Header area is 1 * line_height + 3 pixels tall (extra 3px to prevent sliver)
                            // Use PPA Fill for hardware-accelerated clearing
                            ppa_fill_rect(fb, FB_WIDTH, FB_HEIGHT, 0, MARGIN_TOP, FB_WIDTH, header_height, RGB565_BLACK);
                            
                            float header_vol = 0.0f;
                            if (audio_get_volume(&header_vol) == ESP_OK) {
                                char header_text[128];
                                const char *filename = strrchr(current_mod_path, '/');
                                if (!filename) filename = current_mod_path;
                                else filename++;
                                if (!filename[0]) filename = "Unknown";
                                
                                int vol_percent = (int)(header_vol * 100.0f);
                                int header_len = snprintf(header_text, sizeof(header_text), "%s | Vol: %d%%", filename, vol_percent);
                                if (header_len >= (int)sizeof(header_text)) {
                                    header_text[sizeof(header_text) - 1] = '\0';
                                }
                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, MARGIN_TOP, RGB565_WHITE, 2, header_text);
                            }
                        }
                        
                        // Hex digit lookup table removed - using snprintf with %02X format instead
                        
                        // Note name lookup
                        static const char *note_names[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
                        
                        // Determine if we need compact format based on estimated text width
                        // Full format: "C-4 01 A02" = ~11 chars, Compact: "C4 01 A02" = ~10 chars
                        // Separator: "|" = 1 char
                        // Estimate: full format needs ~12 chars per channel, compact needs ~11 chars per channel
                        // Calculate font_scale first
                        int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                        if (font_scale < 1) font_scale = 1;
                        if (font_scale > 3) font_scale = 3;
                        int estimated_chars_per_channel_full = 12;  // "C-4 01 A02|" 
                        int estimated_chars_per_channel_compact = 11;  // "C4 01 A02|"
                        int estimated_total_width_full = estimated_chars_per_channel_full * num_channels * FONT_WIDTH * font_scale;
                        int estimated_total_width_compact = estimated_chars_per_channel_compact * num_channels * FONT_WIDTH * font_scale;
                        int available_width_pixels = CONTENT_WIDTH;  // 780 pixels
                        bool use_compact_format = (estimated_total_width_full > available_width_pixels);
                        
                        if (!tracker_initialized) {
                            // First render - draw empty rows from top, then first actual row at bottom
                            // The last empty row must be exactly one line_height above the first actual row
                            // First render - full screen (fb_fill already clears all margins)
                            ppa_fill_framebuffer(fb, FB_WIDTH, FB_HEIGHT, RGB565_BLACK);
                            
                            // Draw header on first render (always clear to ensure exact alignment)
                            draw_header();
                            
                            // Calculate the position of the first actual row (at the bottom)
                            int first_actual_row_y = logical_height - MARGIN_BOTTOM - line_height;
                            
                            // Don't draw empty rows - just draw the first actual row at the bottom
                            int y = first_actual_row_y;
                            
                            // Draw current frame data directly (not from history)
                            {
                                int x = MARGIN_LEFT;
                                int max_x = FB_WIDTH - MARGIN_RIGHT;
                                for (int ch = 0; ch < num_channels && ch < XMP_MAX_CHANNELS; ch++) {
                                    if (x >= max_x) break;
                                    
                                    struct xmp_channel_info *ci = &frame_info.channel_info[ch];
                                    
                                    char ch_str[64];
                                    format_channel_string(ch_str, sizeof(ch_str), 
                                                         ci->event.note, ci->event.ins, 
                                                         ci->event.fxt, ci->event.fxp,
                                                         use_compact_format, note_names);
                                    
                                    bool channel_muted = false;
                                    mod_player_get_channel_mute(ch, &channel_muted);
                                    uint16_t ch_color = channel_muted ? RGB565_GRAY : channel_colors_rgb565[ch % XMP_MAX_CHANNELS];
                                    
                                    int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                    if (font_scale < 1) font_scale = 1;
                                    if (font_scale > 3) font_scale = 3;
                                    
                                    int text_width = strlen(ch_str) * FONT_WIDTH * font_scale;
                                    
                                    if (x < max_x) {
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                    }
                                    
                                    x += text_width;
                                    
                                    if (ch < num_channels - 1 && x < max_x) {
                                        const char *separator = use_compact_format ? "|" : "  |  ";
                                        font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, separator);
                                        x += strlen(separator) * FONT_WIDTH * font_scale;
                                    }
                                }
                            }
                            
                            tracker_initialized = true;
                            // Initialize last_row to current row to prevent immediate scrolling on first frame
                            last_row = frame_info.row;
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
                            int scrollable_height = FB_HEIGHT - header_y - MARGIN_BOTTOM;
                            
                            // Track when current row started and its duration
                            static TickType_t row_start_tick = 0;
                            static float current_row_duration_ms = 0.0f;
                            
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
                                TickType_t current_tick = xTaskGetTickCount();
                                TickType_t elapsed_ticks = current_tick - row_start_tick;
                                float elapsed_ms = (float)elapsed_ticks * (1000.0f / configTICK_RATE_HZ);
                                
                                // Calculate scroll position: 0 to line_height over row_duration_ms
                                // This ensures we scroll exactly one line_height over the duration of one pattern row
                                float scroll_progress = elapsed_ms / current_row_duration_ms;
                                if (scroll_progress > 1.0f) scroll_progress = 1.0f;  // Clamp to 1.0
                                
                                smooth_scroll_offset = (int)(scroll_progress * (float)line_height);
                                
                                // If we've completed scrolling this row, add it to history
                                if (smooth_scroll_offset >= line_height) {
                                    smooth_scroll_offset = line_height;  // Clamp to line_height
                                    
                                    // Add the row to history
                                    if (tick_history_count < MAX_TRACKER_ROWS) {
                                        memcpy(tick_history[tick_history_count].channels, pending_tick.channels, sizeof(pending_tick.channels));
                                        tick_history[tick_history_count].pos = pending_tick.pos;
                                        tick_history[tick_history_count].pattern = pending_tick.pattern;
                                        tick_history[tick_history_count].row = pending_tick.row;
                                        tick_history[tick_history_count].num_channels = pending_tick.num_channels;
                                        tick_history_count++;
                                    } else {
                                        // Shift history
                                        memmove(tick_history, tick_history + 1, (MAX_TRACKER_ROWS - 1) * sizeof(struct tracker_tick));
                                        memcpy(tick_history[MAX_TRACKER_ROWS - 1].channels, pending_tick.channels, sizeof(pending_tick.channels));
                                        tick_history[MAX_TRACKER_ROWS - 1].pos = pending_tick.pos;
                                        tick_history[MAX_TRACKER_ROWS - 1].pattern = pending_tick.pattern;
                                        tick_history[MAX_TRACKER_ROWS - 1].row = pending_tick.row;
                                        tick_history[MAX_TRACKER_ROWS - 1].num_channels = pending_tick.num_channels;
                                    }
                                    
                                    // Row completed scrolling - add to history and prepare for next
                                    pending_new_row = false;
                                    smooth_scroll_offset = 0;  // Reset for next row
                                    // last_row is already updated when we detect the new row
                                    
                                    // Immediately check if there's already a new row waiting
                                    // This prevents hitching by starting the next scroll immediately
                                    if (frame_info.row != last_row && !pending_new_row) {
                                        // New row already available - start scrolling it immediately
                                        pending_new_row = true;
                                        memcpy(pending_tick.channels, frame_info.channel_info, sizeof(frame_info.channel_info));
                                        pending_tick.pos = frame_info.pos;
                                        pending_tick.pattern = frame_info.pattern;
                                        pending_tick.row = frame_info.row;
                                        pending_tick.num_channels = num_channels;
                                        
                                        float row_duration_us = (float)frame_info.speed * (float)frame_info.frame_time;
                                        float single_row_duration_ms = row_duration_us / 1000.0f;
                                        const int ROWS_PER_SCROLL = 3;
                                        current_row_duration_ms = single_row_duration_ms * ROWS_PER_SCROLL;
                                        
                                        row_start_tick = xTaskGetTickCount();
                                        smooth_scroll_offset = 0;
                                        last_row = frame_info.row;
                                    }
                                }
                            }
                            
                            // Don't scroll the framebuffer - instead, redraw all visible rows at their correct positions
                            // This prevents double movement and gives us full control
                            
                            // Don't clear the entire area every frame - causes flickering
                            // Instead, we'll clear only the areas that need it as we draw rows
                            
                            // Redraw all visible rows from history at their correct positions
                            // Rows in history are stored chronologically (oldest first, newest last)
                            // We need to draw them from bottom to top, with the most recent at the bottom
                            int num_visible_rows = (scrollable_height + line_height - 1) / line_height + 1;  // +1 for partial row
                            
                            for (int i = 0; i < num_visible_rows && i < tick_history_count; i++) {
                                // Start from most recent row (last in history) and work backwards
                                int history_idx = tick_history_count - 1 - i;
                                if (history_idx < 0) break;
                                
                                // Calculate Y position: most recent row is at bottom, older rows above
                                // Account for smooth_scroll_offset to position correctly
                                // When smooth_scroll_offset is 0, rows are at integer line_height positions
                                // As smooth_scroll_offset increases, rows move UP (subtract offset)
                                int y = logical_height - MARGIN_BOTTOM - line_height - (i * line_height) - smooth_scroll_offset;
                                
                                // Only draw if row is fully below header (y must be >= header_y)
                                // This prevents any pixels from being drawn into the header area
                                if (y >= header_y && y < logical_height - MARGIN_BOTTOM) {
                                    // Clear the row area before drawing to prevent smudging
                                    int row_clear_y = y;
                                    int row_clear_end = (y + line_height > logical_height - MARGIN_BOTTOM) ? (logical_height - MARGIN_BOTTOM) : (y + line_height);
                                    if (row_clear_y < row_clear_end) {
                                        int clear_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                                        int clear_height = row_clear_end - row_clear_y;
                                        ppa_fill_rect(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, row_clear_y, clear_width, clear_height, RGB565_BLACK);
                                    }
                                    
                                    // Draw the row from history
                                    struct tracker_tick *tick = &tick_history[history_idx];
                                    int x = MARGIN_LEFT;
                                    int max_x = FB_WIDTH - MARGIN_RIGHT;
                                    for (int ch = 0; ch < tick->num_channels && ch < XMP_MAX_CHANNELS; ch++) {
                                        if (x >= max_x) break;
                                        
                                        struct xmp_channel_info *ci = &tick->channels[ch];
                                        
                                        char ch_str[64];
                                        format_channel_string(ch_str, sizeof(ch_str), 
                                                             ci->event.note, ci->event.ins, 
                                                             ci->event.fxt, ci->event.fxp,
                                                             use_compact_format, note_names);
                                        
                                        bool channel_muted = false;
                                        mod_player_get_channel_mute(ch, &channel_muted);
                                        uint16_t ch_color = channel_muted ? RGB565_GRAY : channel_colors_rgb565[ch % XMP_MAX_CHANNELS];
                                        
                                        int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                        if (font_scale < 1) font_scale = 1;
                                        if (font_scale > 3) font_scale = 3;
                                        
                                        int text_width = strlen(ch_str) * FONT_WIDTH * font_scale;
                                        
                                        if (x < max_x) {
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                        }
                                        
                                        x += text_width;
                                        
                                        if (ch < tick->num_channels - 1 && x < max_x) {
                                            const char *separator = use_compact_format ? "|" : "  |  ";
                                            font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, separator);
                                            x += strlen(separator) * FONT_WIDTH * font_scale;
                                        }
                                    }
                                }
                            }
                            
                            // Draw the new row progressively as it scrolls in from below
                            // Position it so it appears to scroll up smoothly
                            if (pending_new_row) {
                                    // Calculate where to draw the new row (offset by remaining scroll distance)
                                    // When smooth_scroll_offset is 0, row starts at bottom
                                    // When smooth_scroll_offset is line_height, row is fully scrolled in
                                    int remaining_scroll = line_height - smooth_scroll_offset;
                                    int y = logical_height - MARGIN_BOTTOM - line_height + remaining_scroll;
                                    
                                    // Only draw if the row is fully below header (y must be >= header_y)
                                    // This prevents any pixels from being drawn into the header area
                                    if (y >= header_y && y < logical_height - MARGIN_BOTTOM) {
                                        // Always clear the row area before drawing to ensure clean rendering
                                        int row_clear_y = y;
                                        int row_clear_end = (y + line_height > logical_height - MARGIN_BOTTOM) ? (logical_height - MARGIN_BOTTOM) : (y + line_height);
                                        if (row_clear_y < row_clear_end) {
                                            int clear_width = FB_WIDTH - MARGIN_LEFT - MARGIN_RIGHT;
                                            int clear_height = row_clear_end - row_clear_y;
                                            ppa_fill_rect(fb, FB_WIDTH, FB_HEIGHT, MARGIN_LEFT, row_clear_y, clear_width, clear_height, RGB565_BLACK);
                                        }
                                        
                                        int x = MARGIN_LEFT;
                                        int max_x = FB_WIDTH - MARGIN_RIGHT;
                                        for (int ch = 0; ch < num_channels && ch < XMP_MAX_CHANNELS; ch++) {
                                            if (x >= max_x) break;
                                            
                                            struct xmp_channel_info *ci = &pending_tick.channels[ch];
                                            
                                            char ch_str[64];
                                            format_channel_string(ch_str, sizeof(ch_str), 
                                                                 ci->event.note, ci->event.ins, 
                                                                 ci->event.fxt, ci->event.fxp,
                                                                 use_compact_format, note_names);
                                            
                                            bool channel_muted = false;
                                            mod_player_get_channel_mute(ch, &channel_muted);
                                            uint16_t ch_color = channel_muted ? RGB565_GRAY : channel_colors_rgb565[ch % XMP_MAX_CHANNELS];
                                            
                                            int font_scale = (line_height + FONT_HEIGHT - 1) / FONT_HEIGHT;
                                            if (font_scale < 1) font_scale = 1;
                                            if (font_scale > 3) font_scale = 3;
                                            
                                            int text_width = strlen(ch_str) * FONT_WIDTH * font_scale;
                                            
                                            if (x < max_x) {
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, ch_color, font_scale, ch_str);
                                            }
                                            
                                            x += text_width;
                                            
                                            if (ch < num_channels - 1 && x < max_x) {
                                                const char *separator = use_compact_format ? "|" : "  |  ";
                                                font_draw_string_scaled(fb, FB_WIDTH, FB_HEIGHT, x, y, RGB565_WHITE, font_scale, separator);
                                                x += strlen(separator) * FONT_WIDTH * font_scale;
                                            }
                                        }
                                    }
                            }
                            
                            // No need to draw empty rows - just let the background show through
                            
                            // Always blit to update display (smooth scrolling means we update every frame)
                            PROFILING_START(blit);
                            blit(-1, 0);
                            PROFILING_END(blit, blit);
                        }
                    }
                    
                    PROFILING_END(render, render);
                    // Note: last_row is now updated when we finish scrolling, not immediately
                    // This allows smooth scrolling to complete before marking the row as processed
                }
            }
        }
        
        PROFILING_END_FRAME(frame);
        if (g_profiling_stats.frame_count % 10 == 0) {
            profiling_report();
        }
    }
}
