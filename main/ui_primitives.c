#include "ui_primitives.h"
#include "ui_theme.h"
#include <string.h>

// =============================================================================
// Internal Helpers
// =============================================================================

// Set a single pixel with bounds checking
static inline void set_pixel(uint16_t *fb, int fb_width, int fb_height,
                             int x, int y, uint16_t color) {
    if (x >= 0 && x < fb_width && y >= 0 && y < fb_height) {
        fb[y * fb_width + x] = color;
    }
}

// Clamp value to range
static inline int clamp_int(int val, int min_val, int max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static inline float clamp_float(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

// =============================================================================
// Basic Drawing Primitives
// =============================================================================

void ui_draw_hline(uint16_t *fb, int fb_width, int fb_height,
                   int x, int y, int width, uint16_t color) {
    if (!fb || y < 0 || y >= fb_height || width <= 0) return;

    int x_start = clamp_int(x, 0, fb_width);
    int x_end = clamp_int(x + width, 0, fb_width);
    if (x_start >= x_end) return;

    uint16_t *row = &fb[y * fb_width + x_start];
    int count = x_end - x_start;

    // Optimized fill using 32-bit writes where possible
    if (count >= 4) {
        uint32_t color32 = ((uint32_t)color << 16) | color;
        uint32_t *row32 = (uint32_t *)row;
        int word_count = count / 2;
        for (int i = 0; i < word_count; i++) {
            row32[i] = color32;
        }
        // Handle remaining pixels
        if (count & 1) {
            row[count - 1] = color;
        }
    } else {
        for (int i = 0; i < count; i++) {
            row[i] = color;
        }
    }
}

void ui_draw_vline(uint16_t *fb, int fb_width, int fb_height,
                   int x, int y, int height, uint16_t color) {
    if (!fb || x < 0 || x >= fb_width || height <= 0) return;

    int y_start = clamp_int(y, 0, fb_height);
    int y_end = clamp_int(y + height, 0, fb_height);

    for (int py = y_start; py < y_end; py++) {
        fb[py * fb_width + x] = color;
    }
}

void ui_draw_rect(uint16_t *fb, int fb_width, int fb_height,
                  int x, int y, int width, int height, uint16_t color) {
    if (!fb || width <= 0 || height <= 0) return;

    // Clip to framebuffer bounds
    int x_start = clamp_int(x, 0, fb_width);
    int y_start = clamp_int(y, 0, fb_height);
    int x_end = clamp_int(x + width, 0, fb_width);
    int y_end = clamp_int(y + height, 0, fb_height);

    int clipped_width = x_end - x_start;
    if (clipped_width <= 0) return;

    for (int py = y_start; py < y_end; py++) {
        ui_draw_hline(fb, fb_width, fb_height, x_start, py, clipped_width, color);
    }
}

void ui_draw_rect_outline(uint16_t *fb, int fb_width, int fb_height,
                          int x, int y, int width, int height, uint16_t color) {
    if (!fb || width <= 0 || height <= 0) return;

    // Top and bottom edges
    ui_draw_hline(fb, fb_width, fb_height, x, y, width, color);
    ui_draw_hline(fb, fb_width, fb_height, x, y + height - 1, width, color);

    // Left and right edges (excluding corners already drawn)
    ui_draw_vline(fb, fb_width, fb_height, x, y + 1, height - 2, color);
    ui_draw_vline(fb, fb_width, fb_height, x + width - 1, y + 1, height - 2, color);
}

// =============================================================================
// Rounded Rectangles
// =============================================================================

// Corner mask lookup for small radii (pre-computed for efficiency)
// Each byte represents which pixels to skip in that row from the corner
static const uint8_t corner_mask_r1[] = {1};  // radius 1
static const uint8_t corner_mask_r2[] = {2, 1};  // radius 2
static const uint8_t corner_mask_r3[] = {2, 1, 1};  // radius 3
static const uint8_t corner_mask_r4[] = {3, 2, 1, 1};  // radius 4
static const uint8_t corner_mask_r5[] = {4, 2, 2, 1, 1};  // radius 5
static const uint8_t corner_mask_r6[] = {4, 3, 2, 1, 1, 1};  // radius 6

static const uint8_t *corner_masks[] = {
    NULL,           // radius 0 (no corners)
    corner_mask_r1,
    corner_mask_r2,
    corner_mask_r3,
    corner_mask_r4,
    corner_mask_r5,
    corner_mask_r6
};

void ui_draw_rounded_rect(uint16_t *fb, int fb_width, int fb_height,
                          int x, int y, int width, int height,
                          int radius, uint16_t color) {
    if (!fb || width <= 0 || height <= 0) return;

    // Clamp radius
    int max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;
    if (radius > 6) radius = 6;  // Limit to pre-computed masks
    if (radius < 0) radius = 0;

    if (radius == 0) {
        ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, color);
        return;
    }

    const uint8_t *mask = corner_masks[radius];

    for (int row = 0; row < height; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_height) continue;

        int inset = 0;

        // Top corners
        if (row < radius) {
            inset = mask[row];
        }
        // Bottom corners
        else if (row >= height - radius) {
            inset = mask[height - 1 - row];
        }

        int line_x = x + inset;
        int line_width = width - 2 * inset;

        if (line_width > 0) {
            ui_draw_hline(fb, fb_width, fb_height, line_x, py, line_width, color);
        }
    }
}

void ui_draw_rounded_rect_outline(uint16_t *fb, int fb_width, int fb_height,
                                   int x, int y, int width, int height,
                                   int radius, uint16_t color) {
    if (!fb || width <= 0 || height <= 0) return;

    int max_radius = (width < height ? width : height) / 2;
    if (radius > max_radius) radius = max_radius;
    if (radius > 6) radius = 6;
    if (radius < 0) radius = 0;

    if (radius == 0) {
        ui_draw_rect_outline(fb, fb_width, fb_height, x, y, width, height, color);
        return;
    }

    const uint8_t *mask = corner_masks[radius];

    // Draw top and bottom edges
    ui_draw_hline(fb, fb_width, fb_height, x + radius, y, width - 2 * radius, color);
    ui_draw_hline(fb, fb_width, fb_height, x + radius, y + height - 1, width - 2 * radius, color);

    // Draw left and right edges
    ui_draw_vline(fb, fb_width, fb_height, x, y + radius, height - 2 * radius, color);
    ui_draw_vline(fb, fb_width, fb_height, x + width - 1, y + radius, height - 2 * radius, color);

    // Draw corners using mask
    for (int i = 0; i < radius; i++) {
        int inset = mask[i];
        int prev_inset = (i > 0) ? mask[i - 1] : radius;

        // Top-left corner
        for (int px = x + prev_inset - 1; px >= x + inset; px--) {
            set_pixel(fb, fb_width, fb_height, px, y + i, color);
        }
        if (i < radius - 1) {
            set_pixel(fb, fb_width, fb_height, x + inset - 1, y + i, color);
        }

        // Top-right corner
        for (int px = x + width - prev_inset; px < x + width - inset; px++) {
            set_pixel(fb, fb_width, fb_height, px, y + i, color);
        }
        if (i < radius - 1) {
            set_pixel(fb, fb_width, fb_height, x + width - inset, y + i, color);
        }

        // Bottom-left corner
        int by = y + height - 1 - i;
        for (int px = x + prev_inset - 1; px >= x + inset; px--) {
            set_pixel(fb, fb_width, fb_height, px, by, color);
        }
        if (i < radius - 1) {
            set_pixel(fb, fb_width, fb_height, x + inset - 1, by, color);
        }

        // Bottom-right corner
        for (int px = x + width - prev_inset; px < x + width - inset; px++) {
            set_pixel(fb, fb_width, fb_height, px, by, color);
        }
        if (i < radius - 1) {
            set_pixel(fb, fb_width, fb_height, x + width - inset, by, color);
        }
    }
}

// =============================================================================
// Gradient Fills
// =============================================================================

void ui_draw_hgradient(uint16_t *fb, int fb_width, int fb_height,
                       int x, int y, int width, int height,
                       uint16_t color_start, uint16_t color_end) {
    if (!fb || width <= 0 || height <= 0) return;

    // Pre-compute gradient colors for each column
    for (int col = 0; col < width; col++) {
        int px = x + col;
        if (px < 0 || px >= fb_width) continue;

        float t = (float)col / (float)(width - 1);
        uint16_t color = theme_lerp_color(color_start, color_end, t);

        // Draw vertical stripe
        for (int row = 0; row < height; row++) {
            int py = y + row;
            if (py >= 0 && py < fb_height) {
                fb[py * fb_width + px] = color;
            }
        }
    }
}

void ui_draw_vgradient(uint16_t *fb, int fb_width, int fb_height,
                       int x, int y, int width, int height,
                       uint16_t color_start, uint16_t color_end) {
    if (!fb || width <= 0 || height <= 0) return;

    for (int row = 0; row < height; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_height) continue;

        float t = (float)row / (float)(height - 1);
        uint16_t color = theme_lerp_color(color_start, color_end, t);

        ui_draw_hline(fb, fb_width, fb_height, x, py, width, color);
    }
}

// =============================================================================
// Shadow Effects
// =============================================================================

void ui_draw_shadow_rect(uint16_t *fb, int fb_width, int fb_height,
                         int x, int y, int width, int height,
                         uint16_t color, int shadow_offset, uint16_t shadow_color) {
    if (!fb || width <= 0 || height <= 0) return;

    // Draw shadow first (offset down-right)
    if (shadow_offset > 0) {
        ui_draw_rect(fb, fb_width, fb_height,
                     x + shadow_offset, y + shadow_offset,
                     width, height, shadow_color);
    }

    // Draw main rectangle on top
    ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, color);
}

void ui_draw_shadow_rounded_rect(uint16_t *fb, int fb_width, int fb_height,
                                  int x, int y, int width, int height,
                                  int radius, uint16_t color,
                                  int shadow_offset, uint16_t shadow_color) {
    if (!fb || width <= 0 || height <= 0) return;

    // Draw shadow first
    if (shadow_offset > 0) {
        ui_draw_rounded_rect(fb, fb_width, fb_height,
                             x + shadow_offset, y + shadow_offset,
                             width, height, radius, shadow_color);
    }

    // Draw main rectangle on top
    ui_draw_rounded_rect(fb, fb_width, fb_height, x, y, width, height, radius, color);
}

// =============================================================================
// Progress Bar
// =============================================================================

void ui_draw_progress_bar(uint16_t *fb, int fb_width, int fb_height,
                          int x, int y, int width, int height,
                          float progress, ui_progress_style_t style,
                          uint16_t fg_color, uint16_t bg_color) {
    if (!fb || width <= 0 || height <= 0) return;

    progress = clamp_float(progress, 0.0f, 1.0f);
    int fill_width = (int)(progress * width);

    switch (style) {
        case UI_PROGRESS_STYLE_FLAT:
            // Draw background
            ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, bg_color);
            // Draw progress fill
            if (fill_width > 0) {
                ui_draw_rect(fb, fb_width, fb_height, x, y, fill_width, height, fg_color);
            }
            break;

        case UI_PROGRESS_STYLE_GRADIENT:
            // Draw background
            ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, bg_color);
            // Draw gradient progress fill
            if (fill_width > 0) {
                ui_draw_hgradient(fb, fb_width, fb_height, x, y, fill_width, height,
                                  fg_color, theme_lerp_color(fg_color, THEME_VU_HIGH, 0.5f));
            }
            break;

        case UI_PROGRESS_STYLE_ROUNDED:
            // Draw rounded background
            ui_draw_rounded_rect(fb, fb_width, fb_height, x, y, width, height,
                                 height / 2, bg_color);
            // Draw rounded progress fill
            if (fill_width > height) {  // Need enough width for rounded ends
                ui_draw_rounded_rect(fb, fb_width, fb_height, x, y, fill_width, height,
                                     height / 2, fg_color);
            } else if (fill_width > 0) {
                // Too narrow for rounded - draw partial
                ui_draw_rect(fb, fb_width, fb_height, x, y, fill_width, height, fg_color);
            }
            break;
    }
}

// =============================================================================
// VU Meter
// =============================================================================

void ui_draw_vu_meter(uint16_t *fb, int fb_width, int fb_height,
                      int x, int y, int width, int height,
                      float level, float peak_level,
                      ui_vu_orientation_t orientation) {
    if (!fb || width <= 0 || height <= 0) return;

    level = clamp_float(level, 0.0f, 1.0f);

    // Draw background
    ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, THEME_VU_BG);

    if (orientation == UI_VU_HORIZONTAL) {
        // Horizontal VU meter (left to right)
        int fill_width = (int)(level * width);

        // Draw gradient fill
        for (int col = 0; col < fill_width; col++) {
            float t = (float)col / (float)width;
            uint16_t color = theme_vu_color(t);
            ui_draw_vline(fb, fb_width, fb_height, x + col, y, height, color);
        }

        // Draw peak hold indicator
        if (peak_level >= 0.0f) {
            peak_level = clamp_float(peak_level, 0.0f, 1.0f);
            int peak_x = x + (int)(peak_level * (width - 1));
            ui_draw_vline(fb, fb_width, fb_height, peak_x, y, height, THEME_VU_PEAK);
        }
    } else {
        // Vertical VU meter (bottom to top)
        int fill_height = (int)(level * height);

        // Draw gradient fill from bottom
        for (int row = 0; row < fill_height; row++) {
            int py = y + height - 1 - row;  // Start from bottom
            float t = (float)row / (float)height;
            uint16_t color = theme_vu_color(t);
            ui_draw_hline(fb, fb_width, fb_height, x, py, width, color);
        }

        // Draw peak hold indicator
        if (peak_level >= 0.0f) {
            peak_level = clamp_float(peak_level, 0.0f, 1.0f);
            int peak_y = y + height - 1 - (int)(peak_level * (height - 1));
            ui_draw_hline(fb, fb_width, fb_height, x, peak_y, width, THEME_VU_PEAK);
        }
    }
}

void ui_draw_vu_bar(uint16_t *fb, int fb_width, int fb_height,
                    int x, int y, int width, int height,
                    float level, uint16_t bg_color) {
    if (!fb || width <= 0 || height <= 0) return;

    level = clamp_float(level, 0.0f, 1.0f);
    int fill_width = (int)(level * width);

    // Draw background
    ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, bg_color);

    // Draw gradient fill
    if (fill_width > 0) {
        for (int col = 0; col < fill_width; col++) {
            float t = (float)col / (float)width;
            uint16_t color = theme_vu_color(t);
            ui_draw_vline(fb, fb_width, fb_height, x + col, y, height, color);
        }
    }
}

// =============================================================================
// Spectrum Analyzer Bars
// =============================================================================

void ui_draw_spectrum_bars(uint16_t *fb, int fb_width, int fb_height,
                           int x, int y, int width, int height,
                           const uint8_t *bands, int num_bands,
                           const uint8_t *peaks, int gap) {
    if (!fb || !bands || width <= 0 || height <= 0 || num_bands <= 0) return;

    // Calculate bar width
    int total_gaps = (num_bands - 1) * gap;
    int bar_width = (width - total_gaps) / num_bands;
    if (bar_width < 1) bar_width = 1;

    // Draw background
    ui_draw_rect(fb, fb_width, fb_height, x, y, width, height, THEME_SPECTRUM_BG);

    // Draw each bar
    for (int i = 0; i < num_bands; i++) {
        int bar_x = x + i * (bar_width + gap);
        int bar_height = (bands[i] * height) / 255;
        int bar_y = y + height - bar_height;

        // Draw gradient bar from bottom
        for (int row = 0; row < bar_height; row++) {
            int py = bar_y + row;
            // Color based on height position
            float t = (float)(height - 1 - (py - y)) / (float)height;
            uint16_t color;

            if (t < 0.3f) {
                color = theme_lerp_color(THEME_SPECTRUM_1, THEME_SPECTRUM_2, t / 0.3f);
            } else if (t < 0.5f) {
                color = theme_lerp_color(THEME_SPECTRUM_2, THEME_SPECTRUM_3, (t - 0.3f) / 0.2f);
            } else if (t < 0.7f) {
                color = theme_lerp_color(THEME_SPECTRUM_3, THEME_SPECTRUM_4, (t - 0.5f) / 0.2f);
            } else {
                color = theme_lerp_color(THEME_SPECTRUM_4, THEME_SPECTRUM_5, (t - 0.7f) / 0.3f);
            }

            ui_draw_hline(fb, fb_width, fb_height, bar_x, py, bar_width, color);
        }

        // Draw peak indicator if provided
        if (peaks) {
            int peak_height = (peaks[i] * height) / 255;
            int peak_y = y + height - peak_height;
            if (peak_y >= y && peak_y < y + height) {
                ui_draw_hline(fb, fb_width, fb_height, bar_x, peak_y, bar_width, THEME_SPECTRUM_PEAK);
            }
        }
    }
}

// =============================================================================
// Icon Drawing
// =============================================================================

void ui_draw_icon(uint16_t *fb, int fb_width, int fb_height,
                  int x, int y,
                  const uint8_t *icon_data, int icon_width, int icon_height,
                  uint16_t color) {
    if (!fb || !icon_data || icon_width <= 0 || icon_height <= 0) return;

    int bytes_per_row = (icon_width + 7) / 8;  // Round up to nearest byte

    for (int row = 0; row < icon_height; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_height) continue;

        for (int col = 0; col < icon_width; col++) {
            int px = x + col;
            if (px < 0 || px >= fb_width) continue;

            // Get bit from icon data (MSB first)
            int byte_idx = row * bytes_per_row + col / 8;
            int bit_idx = 7 - (col % 8);

            if (icon_data[byte_idx] & (1 << bit_idx)) {
                fb[py * fb_width + px] = color;
            }
        }
    }
}

void ui_draw_icon_scaled(uint16_t *fb, int fb_width, int fb_height,
                         int x, int y,
                         const uint8_t *icon_data, int icon_width, int icon_height,
                         int scale, uint16_t color) {
    if (!fb || !icon_data || icon_width <= 0 || icon_height <= 0 || scale <= 0) return;

    int bytes_per_row = (icon_width + 7) / 8;

    for (int row = 0; row < icon_height; row++) {
        for (int col = 0; col < icon_width; col++) {
            // Get bit from icon data (MSB first)
            int byte_idx = row * bytes_per_row + col / 8;
            int bit_idx = 7 - (col % 8);

            if (icon_data[byte_idx] & (1 << bit_idx)) {
                // Draw scaled pixel (scale x scale block)
                int px = x + col * scale;
                int py = y + row * scale;
                ui_draw_rect(fb, fb_width, fb_height, px, py, scale, scale, color);
            }
        }
    }
}

// =============================================================================
// Animation Helpers
// =============================================================================

float ui_ease_out(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t);
}

float ui_ease_in_out(float t) {
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    if (t < 0.5f) {
        return 2.0f * t * t;
    } else {
        return 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    }
}

int ui_lerp_int(int a, int b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (int)((b - a) * t);
}
