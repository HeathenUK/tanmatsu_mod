/**
 * @file hw_accel.c
 * @brief Hardware-accelerated graphics operations using PPA
 */

#include "hw_accel.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "bsp/display.h"
#include <string.h>

static const char TAG[] = "hw_accel";

// PPA client handles
static ppa_client_handle_t ppa_srm_handle = NULL;
static ppa_client_handle_t ppa_fill_handle = NULL;

esp_err_t hw_accel_init(void) {
    // Initialize PPA SRM client for rotation/scale/mirror operations
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t ret = ppa_register_client(&ppa_srm_config, &ppa_srm_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA SRM client: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PPA SRM client registered for rotation");

    // Initialize PPA Fill client for hardware-accelerated fills
    ppa_client_config_t ppa_fill_config = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    ret = ppa_register_client(&ppa_fill_config, &ppa_fill_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA Fill client: %s", esp_err_to_name(ret));
        ppa_unregister_client(ppa_srm_handle);
        ppa_srm_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "PPA Fill client registered for hardware-accelerated fills");

    return ESP_OK;
}

void hw_accel_deinit(void) {
    if (ppa_srm_handle) {
        ppa_unregister_client(ppa_srm_handle);
        ppa_srm_handle = NULL;
    }
    if (ppa_fill_handle) {
        ppa_unregister_client(ppa_fill_handle);
        ppa_fill_handle = NULL;
    }
}

ppa_client_handle_t hw_accel_get_srm_handle(void) {
    return ppa_srm_handle;
}

ppa_client_handle_t hw_accel_get_fill_handle(void) {
    return ppa_fill_handle;
}

void IRAM_ATTR hw_accel_fill_framebuffer(uint16_t *fb_ptr, int width, int height, uint16_t color) {
    if (!fb_ptr || width <= 0 || height <= 0) {
        return;
    }

    if (ppa_fill_handle && fb_ptr) {
        color_pixel_argb8888_data_t fill_color = hw_accel_rgb565_to_argb8888(color);

        // Configure PPA Fill operation
        ppa_fill_oper_config_t fill_config = {
            .out = {
                .buffer = fb_ptr,
                .buffer_size = width * height * sizeof(uint16_t),
                .pic_w = width,
                .pic_h = height,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
            },
            .fill_block_w = width,
            .fill_block_h = height,
            .fill_argb_color = fill_color,
            .mode = PPA_TRANS_MODE_BLOCKING,
            .user_data = NULL,
        };

        esp_err_t ret = ppa_do_fill(ppa_fill_handle, &fill_config);
        if (ret == ESP_OK) {
            return;
        }
        ESP_LOGW(TAG, "PPA Fill failed (%s), falling back to CPU fill", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "PPA Fill handle not available, using CPU fill");
    }

    // CPU fallback: optimized 64-bit word-based fill
    uint64_t color_word = ((uint64_t)color << 48) | ((uint64_t)color << 32) |
                          ((uint64_t)color << 16) | (uint64_t)color;

    int total_pixels = width * height;
    uint64_t *fb_words = (uint64_t *)fb_ptr;
    int word_count = total_pixels / 4;

    for (int i = 0; i < word_count; i++) {
        fb_words[i] = color_word;
    }

    int remainder = total_pixels % 4;
    if (remainder > 0) {
        int start_idx = word_count * 4;
        for (int i = 0; i < remainder; i++) {
            fb_ptr[start_idx + i] = color;
        }
    }
}

void IRAM_ATTR hw_accel_fill_rect(uint16_t *fb_ptr, int width, int height,
                                  int x, int y, int w, int h, uint16_t color) {
    if (!fb_ptr || width <= 0 || height <= 0 || w <= 0 || h <= 0) {
        return;
    }

    // Clip rectangle to framebuffer bounds
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
        return;
    }

    if (ppa_fill_handle) {
        color_pixel_argb8888_data_t fill_color = hw_accel_rgb565_to_argb8888(color);

        ppa_fill_oper_config_t fill_config = {
            .out = {
                .buffer = fb_ptr,
                .buffer_size = width * height * sizeof(uint16_t),
                .pic_w = width,
                .pic_h = height,
                .block_offset_x = x,
                .block_offset_y = y,
                .fill_cm = PPA_FILL_COLOR_MODE_RGB565,
            },
            .fill_block_w = w,
            .fill_block_h = h,
            .fill_argb_color = fill_color,
            .mode = PPA_TRANS_MODE_BLOCKING,
            .user_data = NULL,
        };

        esp_err_t ret = ppa_do_fill(ppa_fill_handle, &fill_config);
        if (ret == ESP_OK) {
            return;
        }
        ESP_LOGW(TAG, "PPA Fill rect failed (%s), falling back to CPU fill", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "PPA Fill handle not available, using CPU fill for rect");
    }

    // CPU fallback: optimized 64-bit word-based fill
    uint64_t color_word = ((uint64_t)color << 48) | ((uint64_t)color << 32) |
                          ((uint64_t)color << 16) | (uint64_t)color;

    for (int dy = 0; dy < h; dy++) {
        int fy = y + dy;
        if (fy >= 0 && fy < height) {
            uint16_t *row = &fb_ptr[fy * width + x];
            int word_count = w / 4;
            uint64_t *row_words = (uint64_t *)row;

            for (int i = 0; i < word_count; i++) {
                row_words[i] = color_word;
            }

            int remainder = w % 4;
            if (remainder > 0) {
                int start_idx = word_count * 4;
                for (int i = 0; i < remainder; i++) {
                    row[start_idx + i] = color;
                }
            }
        }
    }
}

void IRAM_ATTR hw_accel_scroll_framebuffer(uint16_t *fb_pixels, int src_y, int dst_y,
                                           int copy_height, int stride) {
    if (!fb_pixels || !ppa_srm_handle || copy_height <= 0) {
        return;
    }

    int copy_width = stride;  // Full width

    // Direct copy from source region to destination using PPA SRM with 0° rotation
    // Since we're scrolling up (src_y > dst_y), the regions don't overlap, so we can copy directly
    ppa_srm_oper_config_t srm_config = {
        .in.buffer = fb_pixels,
        .in.pic_w = stride,
        .in.pic_h = HW_ACCEL_FB_HEIGHT,
        .in.block_w = copy_width,
        .in.block_h = copy_height,
        .in.block_offset_x = 0,
        .in.block_offset_y = src_y,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer = fb_pixels,
        .out.buffer_size = HW_ACCEL_FB_HEIGHT * stride * sizeof(uint16_t),
        .out.pic_w = stride,
        .out.pic_h = HW_ACCEL_FB_HEIGHT,
        .out.block_offset_x = 0,
        .out.block_offset_y = dst_y,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
    if (ret != ESP_OK) {
        // Fallback: memcpy (non-overlapping regions when scrolling up: src_y > dst_y)
        ESP_LOGW(TAG, "PPA scrolling failed (%s), falling back to memcpy", esp_err_to_name(ret));
        uint16_t *src_ptr = fb_pixels + src_y * stride;
        uint16_t *dst_ptr = fb_pixels + dst_y * stride;
        size_t copy_bytes = copy_height * stride * sizeof(uint16_t);
        memcpy(dst_ptr, src_ptr, copy_bytes);
    }
}

void IRAM_ATTR hw_accel_blit(esp_lcd_panel_handle_t panel_handle,
                             uint16_t *fb, uint16_t *fb_rotated,
                             int row_y, int row_height) {
    // BSP handles double buffering, so we just rotate and blit
    // row_y and row_height are currently unused - we always do full screen rotation
    (void)panel_handle;
    (void)row_y;
    (void)row_height;

    if (!fb || !fb_rotated || !ppa_srm_handle) {
        return;
    }

    // Rotate landscape framebuffer (800x480) to portrait (480x800) using PPA
    ppa_srm_oper_config_t srm_config = {
        .in.buffer = fb,
        .in.pic_w = HW_ACCEL_FB_WIDTH,
        .in.pic_h = HW_ACCEL_FB_HEIGHT,
        .in.block_w = HW_ACCEL_FB_WIDTH,
        .in.block_h = HW_ACCEL_FB_HEIGHT,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .out.buffer = fb_rotated,
        .out.buffer_size = 480 * 800 * sizeof(uint16_t),
        .out.pic_w = 480,
        .out.pic_h = 800,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
    if (ret == ESP_OK) {
        // Send rotated buffer to display via BSP (handles vsync and double buffering)
        bsp_display_blit(0, 0, 480, 800, fb_rotated);
    } else {
        ESP_LOGE(TAG, "PPA rotation failed: %s", esp_err_to_name(ret));
    }
}
