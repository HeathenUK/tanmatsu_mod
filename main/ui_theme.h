#pragma once

/**
 * @file ui_theme.h
 * @brief Centralized color/style configuration for tanmatsu_mod UI
 *
 * This file defines the color palette and UI element sizing used throughout
 * the application for consistent visual styling.
 */

#include <stdint.h>

// =============================================================================
// Color Palette (RGB565 format)
// =============================================================================

// Background colors
#define THEME_BG_PRIMARY      0x0000  // Main background (black)
#define THEME_BG_SECONDARY    0x0841  // Slightly lighter (for alternating rows)
#define THEME_BG_HIGHLIGHT    0x2945  // Selection/current row highlight (visible blue)
#define THEME_BG_HEADER       0x0821  // Header accent (dark blue-ish)

// Text colors
#define THEME_TEXT_PRIMARY    0xFFFF  // Main text (white)
#define THEME_TEXT_SECONDARY  0xC618  // Dimmed text (light gray)
#define THEME_TEXT_MUTED      0x8410  // Muted channels/disabled items

// Accent colors for visual emphasis
#define THEME_ACCENT_1        0xF800  // Red (file type: .mod)
#define THEME_ACCENT_2        0x07E0  // Green (file type: .xm)
#define THEME_ACCENT_3        0x001F  // Blue (file type: .s3m)
#define THEME_ACCENT_4        0xFFE0  // Yellow (file type: .it)
#define THEME_ACCENT_5        0xF81F  // Magenta (folder)
#define THEME_ACCENT_6        0x07FF  // Cyan (parent directory)

// VU meter gradient colors
#define THEME_VU_LOW          0x07E0  // Green (0-50%)
#define THEME_VU_MID          0xFFE0  // Yellow (50-75%)
#define THEME_VU_HIGH         0xF800  // Red (75-100%)
#define THEME_VU_PEAK         0xFFFF  // White (peak hold indicator)
#define THEME_VU_BG           0x2104  // Dark background for VU bar

// Progress bar colors
#define THEME_PROGRESS_FG     0x07E0  // Green progress fill
#define THEME_PROGRESS_BG     0x2104  // Dark background
#define THEME_PROGRESS_BORDER 0x4208  // Subtle border

// Spectrum analyzer colors (gradient from bottom to top)
#define THEME_SPECTRUM_1      0x07E0  // Green (low frequencies)
#define THEME_SPECTRUM_2      0x87E0  // Yellow-green
#define THEME_SPECTRUM_3      0xFFE0  // Yellow
#define THEME_SPECTRUM_4      0xFBE0  // Orange
#define THEME_SPECTRUM_5      0xF800  // Red (high frequencies)
#define THEME_SPECTRUM_BG     0x0000  // Black background
#define THEME_SPECTRUM_PEAK   0xFFFF  // White peak hold

// Channel colors for tracker display (16 distinct colors)
#define THEME_CHANNEL_0       0xF800  // Red
#define THEME_CHANNEL_1       0x07E0  // Green
#define THEME_CHANNEL_2       0x001F  // Blue
#define THEME_CHANNEL_3       0xFFE0  // Yellow
#define THEME_CHANNEL_4       0xF81F  // Magenta
#define THEME_CHANNEL_5       0x07FF  // Cyan
#define THEME_CHANNEL_6       0xFC00  // Orange
#define THEME_CHANNEL_7       0x87E0  // Lime
#define THEME_CHANNEL_8       0x041F  // Light Blue
#define THEME_CHANNEL_9       0xF810  // Pink
#define THEME_CHANNEL_10      0x87FF  // Light Cyan
#define THEME_CHANNEL_11      0xFC10  // Light Red
#define THEME_CHANNEL_12      0x87F0  // Light Green
#define THEME_CHANNEL_13      0x841F  // Light Blue
#define THEME_CHANNEL_14      0xFFF0  // Light Yellow
#define THEME_CHANNEL_15      0xC618  // Light Gray

// =============================================================================
// UI Element Sizing
// =============================================================================

// Corner radius for rounded rectangles (in pixels)
#define THEME_CORNER_RADIUS   4

// Drop shadow offset (in pixels)
#define THEME_SHADOW_OFFSET   2
#define THEME_SHADOW_COLOR    0x0000  // Black shadow

// Icon sizing
#define THEME_ICON_SIZE       16      // Standard icon size (16x16 pixels)
#define THEME_ICON_PADDING    4       // Padding around icons

// Row/item heights
#define THEME_ROW_HEIGHT_SM   24      // Small row height
#define THEME_ROW_HEIGHT_MD   32      // Medium row height (default)
#define THEME_ROW_HEIGHT_LG   40      // Large row height

// Header dimensions
#define THEME_HEADER_HEIGHT   40      // Standard header height
#define THEME_HEADER_GRADIENT_H 8     // Height of gradient fade

// VU meter dimensions
#define THEME_VU_HEIGHT       8       // VU meter bar height
#define THEME_VU_PADDING      2       // Padding around VU meter

// Progress bar dimensions
#define THEME_PROGRESS_HEIGHT 6       // Progress bar height
#define THEME_PROGRESS_PADDING 2      // Padding around progress bar

// Margins
#define THEME_MARGIN_SM       5       // Small margin
#define THEME_MARGIN_MD       10      // Medium margin
#define THEME_MARGIN_LG       20      // Large margin

// =============================================================================
// Animation Timing
// =============================================================================

#define THEME_ANIM_DURATION_MS    100   // Standard animation duration
#define THEME_ANIM_VU_DECAY_MS    50    // VU meter decay rate
#define THEME_ANIM_PEAK_HOLD_MS   1000  // Peak hold duration before decay

// =============================================================================
// Helper Macros
// =============================================================================

// Convert RGB888 to RGB565
#define RGB888_TO_RGB565(r, g, b) \
    (((uint16_t)((r) >> 3) << 11) | ((uint16_t)((g) >> 2) << 5) | ((uint16_t)((b) >> 3)))

// Extract components from RGB565
#define RGB565_R(c) (((c) >> 11) & 0x1F)
#define RGB565_G(c) (((c) >> 5) & 0x3F)
#define RGB565_B(c) ((c) & 0x1F)

// Blend two RGB565 colors (alpha 0-255)
static inline uint16_t theme_blend_colors(uint16_t c1, uint16_t c2, uint8_t alpha) {
    uint8_t r1 = RGB565_R(c1), g1 = RGB565_G(c1), b1 = RGB565_B(c1);
    uint8_t r2 = RGB565_R(c2), g2 = RGB565_G(c2), b2 = RGB565_B(c2);
    uint8_t inv_alpha = 255 - alpha;
    uint8_t r = (r1 * alpha + r2 * inv_alpha) / 255;
    uint8_t g = (g1 * alpha + g2 * inv_alpha) / 255;
    uint8_t b = (b1 * alpha + b2 * inv_alpha) / 255;
    return (r << 11) | (g << 5) | b;
}

// Interpolate between two colors (t = 0.0 to 1.0)
static inline uint16_t theme_lerp_color(uint16_t c1, uint16_t c2, float t) {
    if (t <= 0.0f) return c1;
    if (t >= 1.0f) return c2;
    uint8_t alpha = (uint8_t)(t * 255.0f);
    return theme_blend_colors(c2, c1, alpha);
}

// Get VU meter color based on level (0.0 to 1.0)
static inline uint16_t theme_vu_color(float level) {
    if (level < 0.5f) {
        return theme_lerp_color(THEME_VU_LOW, THEME_VU_MID, level * 2.0f);
    } else {
        return theme_lerp_color(THEME_VU_MID, THEME_VU_HIGH, (level - 0.5f) * 2.0f);
    }
}

// Get channel color by index (wraps around for channels > 15)
static inline uint16_t theme_channel_color(int channel) {
    static const uint16_t channel_colors[16] = {
        THEME_CHANNEL_0,  THEME_CHANNEL_1,  THEME_CHANNEL_2,  THEME_CHANNEL_3,
        THEME_CHANNEL_4,  THEME_CHANNEL_5,  THEME_CHANNEL_6,  THEME_CHANNEL_7,
        THEME_CHANNEL_8,  THEME_CHANNEL_9,  THEME_CHANNEL_10, THEME_CHANNEL_11,
        THEME_CHANNEL_12, THEME_CHANNEL_13, THEME_CHANNEL_14, THEME_CHANNEL_15
    };
    return channel_colors[channel & 0x0F];
}
