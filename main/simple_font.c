#include "simple_font.h"
#include <string.h>
#include <stdbool.h>

// Include VGA 8x16 bitmap font data
// Format: 16 bytes per character (one byte per row, MSB is left pixel)
#include "vga_font_8x16.h"

const uint8_t* font_get_char(char c) {
    if (c < 32 || c > 126) {
        return NULL;
    }
    // VGA font: 16 bytes per character (one row per byte)
    return (const uint8_t*)vga_font_8x16_data[c - 32];
}

void font_draw_string(uint16_t *fb, int width, int height, int x, int y, uint16_t color, const char *text) {
    int px = x;
    int py = y;
    
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            px = x;
            py += FONT_HEIGHT;
            continue;
        }
        
        const uint8_t *char_data = font_get_char(*p);
        if (!char_data) {
            char_data = font_get_char('?');  // Use '?' for unsupported chars
        }
        
        if (char_data) {
            // Draw character - VGA font: row-based format (each byte is a row, MSB is left pixel)
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t row_data = char_data[row];
                int fy = py + row;
                if (fy >= 0 && fy < height) {
                    // Draw row: iterate over 8 bits (pixels) from left to right (MSB to LSB)
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        if (row_data & (0x80 >> col)) {  // Check bit from MSB (left) to LSB (right)
                            int fx = px + col;
                            if (fx >= 0 && fx < width) {
                                fb[fy * width + fx] = color;
                            }
                        }
                    }
                }
            }
            px += FONT_WIDTH;
        }
    }
}

void fb_fill(uint16_t *fb, int width, int height, uint16_t color) {
    // SIMD-optimized fill using multi-word writes
    uint64_t color_word = ((uint64_t)color << 48) | ((uint64_t)color << 32) | 
                          ((uint64_t)color << 16) | (uint64_t)color;
    int total_pixels = width * height;
    uint64_t *fb_words = (uint64_t *)fb;
    int word_count = total_pixels / 4;
    
    for (int i = 0; i < word_count; i++) {
        fb_words[i] = color_word;
    }
    
    int remainder = total_pixels % 4;
    if (remainder > 0) {
        int start_idx = word_count * 4;
        for (int i = 0; i < remainder; i++) {
            fb[start_idx + i] = color;
        }
    }
}

void fb_rect(uint16_t *fb, int width, int height, int x, int y, int w, int h, uint16_t color) {
    // SIMD-optimized rectangle fill using multi-word writes
    uint64_t color_word = ((uint64_t)color << 48) | ((uint64_t)color << 32) | 
                          ((uint64_t)color << 16) | (uint64_t)color;
    
    for (int dy = 0; dy < h; dy++) {
        int fy = y + dy;
        if (fy >= 0 && fy < height) {
            uint16_t *row = &fb[fy * width + x];
            int word_count = w / 4;
            uint64_t *row_words = (uint64_t *)row;
            
            // Fill 4 pixels at a time
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

void IRAM_ATTR font_draw_string_scaled(uint16_t *fb, int width, int height, int x, int y, uint16_t color, int scale, const char *text) {
    int px = x;
    int py = y;
    
    // Pre-calculate character bounds for early exit optimization
    int char_width = FONT_WIDTH * scale;
    int char_height = FONT_HEIGHT * scale;
    
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') {
            px = x;
            py += FONT_HEIGHT * scale;
            continue;
        }
        
        // Early exit: skip character if completely outside bounds
        if (px + char_width < 0 || px >= width || py + char_height < 0 || py >= height) {
            px += char_width;
            continue;
        }
        
        const uint8_t *char_data = font_get_char(*p);
        if (!char_data) {
            char_data = font_get_char('?');
        }
        
        if (char_data) {
            // Pre-calculate bounds for this character (clamped to framebuffer)
            int char_x_min = (px < 0) ? 0 : px;
            int char_x_max = (px + char_width > width) ? width : px + char_width;
            int char_y_min = (py < 0) ? 0 : py;
            int char_y_max = (py + char_height > height) ? height : py + char_height;
            
            // VGA font: row-based format (each byte is a row, MSB is left pixel)
            // Optimize for common scale values
            if (scale == 1) {
                // Fast path for scale=1: no scaling needed
                for (int row = 0; row < FONT_HEIGHT; row++) {
                    uint8_t row_data = char_data[row];
                    int fy = py + row;
                    if (fy >= char_y_min && fy < char_y_max) {
                        uint16_t *fb_row = fb + fy * width;
                        // Draw row: iterate over 8 bits (pixels) from left to right (MSB to LSB)
                        for (int col = 0; col < FONT_WIDTH; col++) {
                            if (row_data & (0x80 >> col)) {  // Check bit from MSB (left) to LSB (right)
                                int fx = px + col;
                                if (fx >= char_x_min && fx < char_x_max) {
                                    fb_row[fx] = color;
                                }
                            }
                        }
                    }
                }
            } else if (scale == 2) {
                // Optimized path for scale=2: unroll inner loops, bounds already checked
                for (int row = 0; row < FONT_HEIGHT; row++) {
                    uint8_t row_data = char_data[row];
                    int row_base_y = py + row * 2;
                    // Check if this row overlaps with visible area
                    if (row_base_y + 2 > char_y_min && row_base_y < char_y_max) {
                        for (int col = 0; col < FONT_WIDTH; col++) {
                            if (row_data & (0x80 >> col)) {  // Check bit from MSB (left) to LSB (right)
                                int col_base_x = px + col * 2;
                                // Check if this column overlaps with visible area
                                if (col_base_x + 2 > char_x_min && col_base_x < char_x_max) {
                                    // Unroll 2x2 block - bounds already verified above
                                    int fx0 = col_base_x;
                                    int fx1 = col_base_x + 1;
                                    int fy0 = row_base_y;
                                    int fy1 = row_base_y + 1;
                                    
                                    // Only check individual pixels if block might be partially clipped
                                    bool fully_visible = (fx0 >= char_x_min && fx1 < char_x_max && 
                                                          fy0 >= char_y_min && fy1 < char_y_max);
                                    if (fully_visible) {
                                        // Fast path: all 4 pixels visible, no bounds checks needed
                                        fb[fy0 * width + fx0] = color;
                                        fb[fy0 * width + fx1] = color;
                                        fb[fy1 * width + fx0] = color;
                                        fb[fy1 * width + fx1] = color;
                                    } else {
                                        // Clipped path: check each pixel
                                        if (fx0 >= char_x_min && fx0 < char_x_max && fy0 >= char_y_min && fy0 < char_y_max) {
                                            fb[fy0 * width + fx0] = color;
                                        }
                                        if (fx1 >= char_x_min && fx1 < char_x_max && fy0 >= char_y_min && fy0 < char_y_max) {
                                            fb[fy0 * width + fx1] = color;
                                        }
                                        if (fx0 >= char_x_min && fx0 < char_x_max && fy1 >= char_y_min && fy1 < char_y_max) {
                                            fb[fy1 * width + fx0] = color;
                                        }
                                        if (fx1 >= char_x_min && fx1 < char_x_max && fy1 >= char_y_min && fy1 < char_y_max) {
                                            fb[fy1 * width + fx1] = color;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // General case for scale >= 3: optimized with pre-calculated bounds and cache-friendly access
                // Use row-major access pattern for better cache locality
                for (int row = 0; row < FONT_HEIGHT; row++) {
                    uint8_t row_data = char_data[row];
                    int row_base_y = py + row * scale;
                    // Check if this row overlaps with visible area
                    if (row_base_y + scale > char_y_min && row_base_y < char_y_max) {
                        for (int col = 0; col < FONT_WIDTH; col++) {
                            if (row_data & (0x80 >> col)) {  // Check bit from MSB (left) to LSB (right)
                                int col_base_x = px + col * scale;
                                // Check if this column overlaps with visible area
                                if (col_base_x + scale > char_x_min && col_base_x < char_x_max) {
                                    // Draw scale x scale block with optimized bounds
                                    // Clamp to visible area for cache-friendly row-major access
                                    int sx_start = (char_x_min > col_base_x) ? (char_x_min - col_base_x) : 0;
                                    int sx_end = (char_x_max < col_base_x + scale) ? (char_x_max - col_base_x) : scale;
                                    int sy_start = (char_y_min > row_base_y) ? (char_y_min - row_base_y) : 0;
                                    int sy_end = (char_y_max < row_base_y + scale) ? (char_y_max - row_base_y) : scale;
                                    
                                    // General case: SIMD-optimized with multi-word writes for horizontal fills
                                    // Replicate color to 64-bit word for efficient filling (4 pixels per word)
                                    uint64_t color_word = ((uint64_t)color << 48) | ((uint64_t)color << 32) | 
                                                          ((uint64_t)color << 16) | (uint64_t)color;
                                    
                                    int block_width = sx_end - sx_start;
                                    for (int sy = sy_start; sy < sy_end; sy++) {
                                        int fy = row_base_y + sy;
                                        uint16_t *fb_row = fb + fy * width;
                                        int row_start_x = col_base_x + sx_start;
                                        
                                        // Fill 4 pixels at a time using 64-bit writes (SIMD-friendly)
                                        int word_count = block_width / 4;
                                        uint64_t *row_words = (uint64_t *)(fb_row + row_start_x);
                                        for (int i = 0; i < word_count; i++) {
                                            row_words[i] = color_word;
                                        }
                                        
                                        // Handle remaining pixels (0-3 pixels)
                                        int remainder = block_width % 4;
                                        if (remainder > 0) {
                                            int remainder_start = row_start_x + word_count * 4;
                                            for (int i = 0; i < remainder; i++) {
                                                fb_row[remainder_start + i] = color;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            px += char_width;
        }
    }
}
