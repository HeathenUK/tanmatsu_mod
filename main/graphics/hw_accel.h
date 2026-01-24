/**
 * @file hw_accel.h
 * @brief Hardware-accelerated graphics operations using PPA (Pixel Processing Accelerator)
 *
 * This module provides hardware-accelerated graphics primitives including:
 * - Color format conversion (RGB565 <-> ARGB8888)
 * - Framebuffer filling and rectangle drawing using PPA Fill
 * - Framebuffer scrolling using PPA SRM (Scale-Rotate-Mirror)
 * - Display rotation and blitting using PPA SRM
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/ppa.h"
#include "hal/color_types.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Framebuffer dimensions (logical landscape orientation)
#define HW_ACCEL_FB_WIDTH  800
#define HW_ACCEL_FB_HEIGHT 480

// RGB565 color definitions
#define RGB565_BLACK       0x0000
#define RGB565_WHITE       0xFFFF
#define RGB565_RED         0xF800
#define RGB565_GREEN       0x07E0
#define RGB565_BLUE        0x001F
#define RGB565_YELLOW      0xFFE0
#define RGB565_MAGENTA     0xF81F
#define RGB565_CYAN        0x07FF
#define RGB565_GRAY        0x8410
#define RGB565_DARK_BLUE   0x1084

/**
 * @brief Initialize hardware acceleration (PPA clients)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t hw_accel_init(void);

/**
 * @brief Deinitialize hardware acceleration
 */
void hw_accel_deinit(void);

/**
 * @brief Get PPA SRM (Scale-Rotate-Mirror) handle for rotation operations
 *
 * @return PPA SRM handle, or NULL if not initialized
 */
ppa_client_handle_t hw_accel_get_srm_handle(void);

/**
 * @brief Get PPA Fill handle for fill operations
 *
 * @return PPA Fill handle, or NULL if not initialized
 */
ppa_client_handle_t hw_accel_get_fill_handle(void);

/**
 * @brief Convert ARGB32 color to RGB565 format
 *
 * @param argb ARGB32 color (0xAARRGGBB)
 * @return RGB565 color (0bRRRRRGGGGGGBBBBB)
 */
static inline uint16_t hw_accel_argb32_to_rgb565(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/**
 * @brief Convert RGB565 color to ARGB8888 format (for PPA API)
 *
 * @param rgb565 RGB565 color
 * @return ARGB8888 color structure
 */
static inline color_pixel_argb8888_data_t hw_accel_rgb565_to_argb8888(uint16_t rgb565) {
    color_pixel_argb8888_data_t argb;
    uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits (scale by 8)
    uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits (scale by 4)
    uint8_t b = (rgb565 & 0x1F) << 3;          // 5 bits -> 8 bits (scale by 8)
    argb.a = 0xFF;
    argb.r = r;
    argb.g = g;
    argb.b = b;
    return argb;
}

/**
 * @brief Fill entire framebuffer with a solid color using PPA hardware acceleration
 *
 * Falls back to optimized CPU fill if PPA is unavailable.
 *
 * @param fb_ptr Pointer to framebuffer
 * @param width Framebuffer width in pixels
 * @param height Framebuffer height in pixels
 * @param color RGB565 color to fill with
 */
void hw_accel_fill_framebuffer(uint16_t *fb_ptr, int width, int height, uint16_t color);

/**
 * @brief Fill a rectangle in the framebuffer using PPA hardware acceleration
 *
 * Falls back to optimized CPU fill if PPA is unavailable.
 * Handles clipping automatically.
 *
 * @param fb_ptr Pointer to framebuffer
 * @param width Framebuffer width in pixels
 * @param height Framebuffer height in pixels
 * @param x Rectangle X position
 * @param y Rectangle Y position
 * @param w Rectangle width
 * @param h Rectangle height
 * @param color RGB565 color to fill with
 */
void hw_accel_fill_rect(uint16_t *fb_ptr, int width, int height,
                        int x, int y, int w, int h, uint16_t color);

/**
 * @brief Scroll framebuffer up using PPA SRM with direct copy
 *
 * Efficiently scrolls a region upward within the framebuffer.
 * Since src_y > dst_y (scrolling up), regions don't overlap, so we can copy directly.
 *
 * @param fb_pixels Pointer to framebuffer
 * @param src_y Source Y position (top of region to copy)
 * @param dst_y Destination Y position (where to copy to)
 * @param copy_height Height of region to copy in pixels
 * @param stride Width of framebuffer in pixels
 */
void hw_accel_scroll_framebuffer(uint16_t *fb_pixels, int src_y, int dst_y,
                                 int copy_height, int stride);

/**
 * @brief Rotate and blit framebuffer to display using PPA SRM
 *
 * Rotates the logical landscape framebuffer (800x480) by 270° CCW to match
 * the physical portrait display (480x800).
 *
 * @param panel_handle LCD panel handle for output
 * @param fb Source framebuffer (800x480 landscape)
 * @param fb_rotated Destination rotated framebuffer (480x800 portrait)
 * @param row_y Optional: Y position of specific row to blit (-1 for full framebuffer)
 * @param row_height Optional: Height of specific row to blit (0 for full framebuffer)
 */
void hw_accel_blit(esp_lcd_panel_handle_t panel_handle,
                   uint16_t *fb, uint16_t *fb_rotated,
                   int row_y, int row_height);

#ifdef __cplusplus
}
#endif
