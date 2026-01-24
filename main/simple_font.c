#include "simple_font.h"
#include <string.h>
#include <stdbool.h>

// Include VGA 8x16 bitmap font data
// Format: 16 bytes per character (one byte per row, MSB is left pixel)
#include "vga_font_8x16.h"

const uint8_t* font_get_char(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 32 && uc <= 126) {
        // Standard ASCII printable characters
        return (const uint8_t*)vga_font_8x16_data[uc - 32];
    } else if (uc >= 128 && uc <= 133) {
        // Custom glyphs: 128=up, 129=down, 130=left, 131=right, 132=return, 133=space bar
        return (const uint8_t*)custom_glyphs[uc - 128];
    }
    // Return '?' for unsupported characters
    return (const uint8_t*)vga_font_8x16_data['?' - 32];
}

typedef enum {
    ACCENT_NONE = 0,
    ACCENT_ACUTE,
    ACCENT_GRAVE,
    ACCENT_CIRCUMFLEX,
    ACCENT_UMLAUT,
    ACCENT_TILDE,
    ACCENT_RING,
    ACCENT_CEDILLA
} accent_t;

static void apply_accent(uint8_t *glyph, accent_t accent) {
    switch (accent) {
        case ACCENT_ACUTE:
            glyph[0] |= 0x0C;
            glyph[1] |= 0x18;
            break;
        case ACCENT_GRAVE:
            glyph[0] |= 0x30;
            glyph[1] |= 0x18;
            break;
        case ACCENT_CIRCUMFLEX:
            glyph[0] |= 0x18;
            glyph[1] |= 0x24;
            glyph[2] |= 0x42;
            break;
        case ACCENT_UMLAUT:
            glyph[0] |= 0x66;
            break;
        case ACCENT_TILDE:
            glyph[0] |= 0x36;
            glyph[1] |= 0x6C;
            break;
        case ACCENT_RING:
            glyph[0] |= 0x18;
            glyph[1] |= 0x24;
            glyph[2] |= 0x18;
            break;
        case ACCENT_CEDILLA:
            glyph[13] |= 0x18;
            glyph[14] |= 0x30;
            break;
        default:
            break;
    }
}

static const uint8_t *font_get_glyph(uint32_t codepoint, uint8_t *scratch) {
    if (codepoint <= 0x7F) {
        return font_get_char((char)codepoint);
    }

    if (codepoint >= 0x80 && codepoint <= 0x85) {
        return (const uint8_t *)custom_glyphs[codepoint - 0x80];
    }

    // Latin-1 supplement accents (UTF-8 decoded)
    char base = 0;
    accent_t accent = ACCENT_NONE;
    switch (codepoint) {
        case 0x00E0: base = 'a'; accent = ACCENT_GRAVE; break;       // à
        case 0x00E1: base = 'a'; accent = ACCENT_ACUTE; break;       // á
        case 0x00E2: base = 'a'; accent = ACCENT_CIRCUMFLEX; break;  // â
        case 0x00E3: base = 'a'; accent = ACCENT_TILDE; break;       // ã
        case 0x00E4: base = 'a'; accent = ACCENT_UMLAUT; break;      // ä
        case 0x00E5: base = 'a'; accent = ACCENT_RING; break;        // å
        case 0x00C0: base = 'A'; accent = ACCENT_GRAVE; break;       // À
        case 0x00C1: base = 'A'; accent = ACCENT_ACUTE; break;       // Á
        case 0x00C2: base = 'A'; accent = ACCENT_CIRCUMFLEX; break;  // Â
        case 0x00C3: base = 'A'; accent = ACCENT_TILDE; break;       // Ã
        case 0x00C4: base = 'A'; accent = ACCENT_UMLAUT; break;      // Ä
        case 0x00C5: base = 'A'; accent = ACCENT_RING; break;        // Å
        case 0x00E8: base = 'e'; accent = ACCENT_GRAVE; break;       // è
        case 0x00E9: base = 'e'; accent = ACCENT_ACUTE; break;       // é
        case 0x00EA: base = 'e'; accent = ACCENT_CIRCUMFLEX; break;  // ê
        case 0x00EB: base = 'e'; accent = ACCENT_UMLAUT; break;      // ë
        case 0x00C8: base = 'E'; accent = ACCENT_GRAVE; break;       // È
        case 0x00C9: base = 'E'; accent = ACCENT_ACUTE; break;       // É
        case 0x00CA: base = 'E'; accent = ACCENT_CIRCUMFLEX; break;  // Ê
        case 0x00CB: base = 'E'; accent = ACCENT_UMLAUT; break;      // Ë
        case 0x00EC: base = 'i'; accent = ACCENT_GRAVE; break;       // ì
        case 0x00ED: base = 'i'; accent = ACCENT_ACUTE; break;       // í
        case 0x00EE: base = 'i'; accent = ACCENT_CIRCUMFLEX; break;  // î
        case 0x00EF: base = 'i'; accent = ACCENT_UMLAUT; break;      // ï
        case 0x00CC: base = 'I'; accent = ACCENT_GRAVE; break;       // Ì
        case 0x00CD: base = 'I'; accent = ACCENT_ACUTE; break;       // Í
        case 0x00CE: base = 'I'; accent = ACCENT_CIRCUMFLEX; break;  // Î
        case 0x00CF: base = 'I'; accent = ACCENT_UMLAUT; break;      // Ï
        case 0x00F2: base = 'o'; accent = ACCENT_GRAVE; break;       // ò
        case 0x00F3: base = 'o'; accent = ACCENT_ACUTE; break;       // ó
        case 0x00F4: base = 'o'; accent = ACCENT_CIRCUMFLEX; break;  // ô
        case 0x00F5: base = 'o'; accent = ACCENT_TILDE; break;       // õ
        case 0x00F6: base = 'o'; accent = ACCENT_UMLAUT; break;      // ö
        case 0x00D2: base = 'O'; accent = ACCENT_GRAVE; break;       // Ò
        case 0x00D3: base = 'O'; accent = ACCENT_ACUTE; break;       // Ó
        case 0x00D4: base = 'O'; accent = ACCENT_CIRCUMFLEX; break;  // Ô
        case 0x00D5: base = 'O'; accent = ACCENT_TILDE; break;       // Õ
        case 0x00D6: base = 'O'; accent = ACCENT_UMLAUT; break;      // Ö
        case 0x00F9: base = 'u'; accent = ACCENT_GRAVE; break;       // ù
        case 0x00FA: base = 'u'; accent = ACCENT_ACUTE; break;       // ú
        case 0x00FB: base = 'u'; accent = ACCENT_CIRCUMFLEX; break;  // û
        case 0x00FC: base = 'u'; accent = ACCENT_UMLAUT; break;      // ü
        case 0x00D9: base = 'U'; accent = ACCENT_GRAVE; break;       // Ù
        case 0x00DA: base = 'U'; accent = ACCENT_ACUTE; break;       // Ú
        case 0x00DB: base = 'U'; accent = ACCENT_CIRCUMFLEX; break;  // Û
        case 0x00DC: base = 'U'; accent = ACCENT_UMLAUT; break;      // Ü
        case 0x00F1: base = 'n'; accent = ACCENT_TILDE; break;       // ñ
        case 0x00D1: base = 'N'; accent = ACCENT_TILDE; break;       // Ñ
        case 0x00E7: base = 'c'; accent = ACCENT_CEDILLA; break;     // ç
        case 0x00C7: base = 'C'; accent = ACCENT_CEDILLA; break;     // Ç
        default:
            return font_get_char('?');
    }

    const uint8_t *base_glyph = font_get_char(base);
    if (!base_glyph) {
        return font_get_char('?');
    }
    memcpy(scratch, base_glyph, FONT_HEIGHT);
    apply_accent(scratch, accent);
    return scratch;
}

static uint32_t font_next_codepoint(const char **text) {
    static uint32_t pending[2];
    static int pending_len = 0;
    static int pending_idx = 0;

    if (pending_idx < pending_len) {
        return pending[pending_idx++];
    }
    pending_len = 0;
    pending_idx = 0;

    const unsigned char *s = (const unsigned char *)(*text);
    if (*s == '\0') {
        return 0;
    }
    if (*s < 0x80) {
        (*text)++;
        return *s;
    }
    if (*s < 0xC0) {
        // Treat raw 0x80-0xBF bytes as custom glyphs
        (*text)++;
        return *s;
    }

    uint32_t cp = 0;
    if (*s >= 0xC2 && *s <= 0xDF && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        (*text) += 2;
    } else if (*s >= 0xE0 && *s <= 0xEF &&
               (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) |
             (uint32_t)(s[2] & 0x3F);
        (*text) += 3;
    } else {
        // Unsupported sequence: skip one byte
        (*text)++;
        return '?';
    }

    switch (cp) {
        case 0x00DF:  // ß
            pending[0] = 's';
            pending[1] = 's';
            pending_len = 2;
            return pending[pending_idx++];
        case 0x00C6:  // Æ
            pending[0] = 'A';
            pending[1] = 'E';
            pending_len = 2;
            return pending[pending_idx++];
        case 0x00E6:  // æ
            pending[0] = 'a';
            pending[1] = 'e';
            pending_len = 2;
            return pending[pending_idx++];
        case 0x0152:  // Œ
            pending[0] = 'O';
            pending[1] = 'E';
            pending_len = 2;
            return pending[pending_idx++];
        case 0x0153:  // œ
            pending[0] = 'o';
            pending[1] = 'e';
            pending_len = 2;
            return pending[pending_idx++];
        case 0x2013:  // –
        case 0x2014:  // —
            return '-';
        case 0x2018:  // ‘
        case 0x2019:  // ’
            return '\'';
        case 0x201C:  // “
        case 0x201D:  // ”
            return '"';
        case 0x2026:  // …
            pending[0] = '.';
            pending[1] = '.';
            pending_len = 2;
            return '.';
        case 0x00A0:  // non-breaking space
            return ' ';
        case 0x00D8:  // Ø
            return 'O';
        case 0x00F8:  // ø
            return 'o';
        case 0x00B0:  // °
            return 'o';
        default:
            return cp;
    }
}

void font_draw_string(uint16_t *fb, int width, int height, int x, int y, uint16_t color, const char *text) {
    int px = x;
    int py = y;
    
    uint8_t scratch[FONT_HEIGHT];
    const char *p = text;
    while (*p != '\0') {
        uint32_t cp = font_next_codepoint(&p);
        if (cp == 0) {
            break;
        }
        if (cp == '\n') {
            px = x;
            py += FONT_HEIGHT;
            continue;
        }

        const uint8_t *char_data = font_get_glyph(cp, scratch);
        
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
    
    uint8_t scratch[FONT_HEIGHT];
    const char *p = text;
    while (*p != '\0') {
        uint32_t cp = font_next_codepoint(&p);
        if (cp == 0) {
            break;
        }
        if (cp == '\n') {
            px = x;
            py += FONT_HEIGHT * scale;
            continue;
        }
        
        // Early exit: skip character if completely outside bounds
        if (px + char_width < 0 || px >= width || py + char_height < 0 || py >= height) {
            px += char_width;
            continue;
        }
        
        const uint8_t *char_data = font_get_glyph(cp, scratch);
        
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
