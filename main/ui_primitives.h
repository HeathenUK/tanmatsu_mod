#pragma once

/**
 * @file ui_primitives.h
 * @brief Enhanced drawing primitives for tanmatsu_mod UI
 *
 * Provides high-level drawing functions for UI elements like rounded rectangles,
 * gradients, progress bars, VU meters, and shadows.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// =============================================================================
// Basic Drawing Primitives
// =============================================================================

/**
 * @brief Draw a horizontal line
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Starting X coordinate
 * @param y Y coordinate
 * @param width Line width
 * @param color RGB565 color
 */
void ui_draw_hline(uint16_t *fb, int fb_width, int fb_height,
                   int x, int y, int width, uint16_t color);

/**
 * @brief Draw a vertical line
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x X coordinate
 * @param y Starting Y coordinate
 * @param height Line height
 * @param color RGB565 color
 */
void ui_draw_vline(uint16_t *fb, int fb_width, int fb_height,
                   int x, int y, int height, uint16_t color);

/**
 * @brief Draw a filled rectangle
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color RGB565 fill color
 */
void ui_draw_rect(uint16_t *fb, int fb_width, int fb_height,
                  int x, int y, int width, int height, uint16_t color);

/**
 * @brief Draw a rectangle outline (unfilled)
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color RGB565 border color
 */
void ui_draw_rect_outline(uint16_t *fb, int fb_width, int fb_height,
                          int x, int y, int width, int height, uint16_t color);

// =============================================================================
// Rounded Rectangles
// =============================================================================

/**
 * @brief Draw a filled rounded rectangle
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param radius Corner radius (clamped to min(width, height) / 2)
 * @param color RGB565 fill color
 */
void ui_draw_rounded_rect(uint16_t *fb, int fb_width, int fb_height,
                          int x, int y, int width, int height,
                          int radius, uint16_t color);

/**
 * @brief Draw a rounded rectangle outline
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param radius Corner radius
 * @param color RGB565 border color
 */
void ui_draw_rounded_rect_outline(uint16_t *fb, int fb_width, int fb_height,
                                   int x, int y, int width, int height,
                                   int radius, uint16_t color);

// =============================================================================
// Gradient Fills
// =============================================================================

/**
 * @brief Draw a horizontal gradient fill
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Gradient width
 * @param height Gradient height
 * @param color_start Starting color (left)
 * @param color_end Ending color (right)
 */
void ui_draw_hgradient(uint16_t *fb, int fb_width, int fb_height,
                       int x, int y, int width, int height,
                       uint16_t color_start, uint16_t color_end);

/**
 * @brief Draw a vertical gradient fill
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Gradient width
 * @param height Gradient height
 * @param color_start Starting color (top)
 * @param color_end Ending color (bottom)
 */
void ui_draw_vgradient(uint16_t *fb, int fb_width, int fb_height,
                       int x, int y, int width, int height,
                       uint16_t color_start, uint16_t color_end);

// =============================================================================
// Shadow Effects
// =============================================================================

/**
 * @brief Draw a rectangle with drop shadow
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param color RGB565 fill color
 * @param shadow_offset Shadow offset in pixels
 * @param shadow_color Shadow color (usually dark/black)
 */
void ui_draw_shadow_rect(uint16_t *fb, int fb_width, int fb_height,
                         int x, int y, int width, int height,
                         uint16_t color, int shadow_offset, uint16_t shadow_color);

/**
 * @brief Draw a rounded rectangle with drop shadow
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Rectangle width
 * @param height Rectangle height
 * @param radius Corner radius
 * @param color RGB565 fill color
 * @param shadow_offset Shadow offset in pixels
 * @param shadow_color Shadow color
 */
void ui_draw_shadow_rounded_rect(uint16_t *fb, int fb_width, int fb_height,
                                  int x, int y, int width, int height,
                                  int radius, uint16_t color,
                                  int shadow_offset, uint16_t shadow_color);

// =============================================================================
// Progress Bar
// =============================================================================

/**
 * @brief Progress bar style options
 */
typedef enum {
    UI_PROGRESS_STYLE_FLAT,     // Solid color fill
    UI_PROGRESS_STYLE_GRADIENT, // Horizontal gradient
    UI_PROGRESS_STYLE_ROUNDED   // Rounded corners with flat fill
} ui_progress_style_t;

/**
 * @brief Draw a progress bar
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Total bar width
 * @param height Bar height
 * @param progress Progress value (0.0 to 1.0)
 * @param style Visual style
 * @param fg_color Foreground (progress) color
 * @param bg_color Background color
 */
void ui_draw_progress_bar(uint16_t *fb, int fb_width, int fb_height,
                          int x, int y, int width, int height,
                          float progress, ui_progress_style_t style,
                          uint16_t fg_color, uint16_t bg_color);

// =============================================================================
// VU Meter
// =============================================================================

/**
 * @brief VU meter orientation
 */
typedef enum {
    UI_VU_HORIZONTAL,  // Left to right
    UI_VU_VERTICAL     // Bottom to top
} ui_vu_orientation_t;

/**
 * @brief Draw a VU meter with green->yellow->red gradient
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Meter width
 * @param height Meter height
 * @param level Current level (0.0 to 1.0)
 * @param peak_level Peak level for hold indicator (-1.0 to disable)
 * @param orientation Horizontal or vertical
 */
void ui_draw_vu_meter(uint16_t *fb, int fb_width, int fb_height,
                      int x, int y, int width, int height,
                      float level, float peak_level,
                      ui_vu_orientation_t orientation);

/**
 * @brief Draw a simple horizontal VU bar (no peak, just colored bar)
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Bar width
 * @param height Bar height
 * @param level Current level (0.0 to 1.0)
 * @param bg_color Background color
 */
void ui_draw_vu_bar(uint16_t *fb, int fb_width, int fb_height,
                    int x, int y, int width, int height,
                    float level, uint16_t bg_color);

// =============================================================================
// Spectrum Analyzer Bars
// =============================================================================

/**
 * @brief Draw spectrum analyzer bars
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param width Total width
 * @param height Total height
 * @param bands Array of band levels (0-255 each)
 * @param num_bands Number of bands
 * @param peaks Array of peak levels (NULL to disable peak hold)
 * @param gap Width of gap between bars
 */
void ui_draw_spectrum_bars(uint16_t *fb, int fb_width, int fb_height,
                           int x, int y, int width, int height,
                           const uint8_t *bands, int num_bands,
                           const uint8_t *peaks, int gap);

// =============================================================================
// Icon Drawing
// =============================================================================

/**
 * @brief Draw a 1-bit icon with specified color
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param icon_data Pointer to icon bitmap data (row-major, MSB first)
 * @param icon_width Icon width in pixels
 * @param icon_height Icon height in pixels
 * @param color RGB565 color for set pixels
 */
void ui_draw_icon(uint16_t *fb, int fb_width, int fb_height,
                  int x, int y,
                  const uint8_t *icon_data, int icon_width, int icon_height,
                  uint16_t color);

/**
 * @brief Draw a scaled 1-bit icon
 * @param fb Framebuffer pointer
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x Top-left X coordinate
 * @param y Top-left Y coordinate
 * @param icon_data Pointer to icon bitmap data
 * @param icon_width Icon width in pixels
 * @param icon_height Icon height in pixels
 * @param scale Scale factor (1, 2, 3, etc.)
 * @param color RGB565 color for set pixels
 */
void ui_draw_icon_scaled(uint16_t *fb, int fb_width, int fb_height,
                         int x, int y,
                         const uint8_t *icon_data, int icon_width, int icon_height,
                         int scale, uint16_t color);

// =============================================================================
// Animation Helpers
// =============================================================================

/**
 * @brief Ease-out interpolation (deceleration)
 * @param t Progress value (0.0 to 1.0)
 * @return Eased value (0.0 to 1.0)
 */
float ui_ease_out(float t);

/**
 * @brief Ease-in-out interpolation
 * @param t Progress value (0.0 to 1.0)
 * @return Eased value (0.0 to 1.0)
 */
float ui_ease_in_out(float t);

/**
 * @brief Linear interpolation between two integers
 * @param a Start value
 * @param b End value
 * @param t Progress (0.0 to 1.0)
 * @return Interpolated value
 */
int ui_lerp_int(int a, int b, float t);
