#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_attr.h"  // For IRAM_ATTR

// VGA 8x16 bitmap font (ASCII 32-126)
// Each character is 8 pixels wide, 16 pixels tall
// Stored as 16 bytes per character (one byte per row, MSB is left pixel)
#define FONT_WIDTH 8
#define FONT_HEIGHT 16

// Get font bitmap for a character (returns NULL for unsupported characters)
const uint8_t* font_get_char(char c);

// Render a string to framebuffer at position (x, y) with color
// fb: framebuffer (uint16_t* for RGB565)
// width: framebuffer width in pixels
// height: framebuffer height in pixels
// x, y: position to draw at (top-left corner)
// color: RGB565 color value
// text: null-terminated string to render
void font_draw_string(uint16_t *fb, int width, int height, int x, int y, uint16_t color, const char *text);

// Render a string with scaling (scale factor: 1=8x16, 2=16x32, etc.)
// Hardware-accelerated where possible (PPA for large blocks, optimized loops for small blocks)
void IRAM_ATTR font_draw_string_scaled(uint16_t *fb, int width, int height, int x, int y, uint16_t color, int scale, const char *text);

// NOTE: fb_fill() and fb_rect() are deprecated - use PPA functions in main.c instead:
// - ppa_fill_framebuffer() for full framebuffer fills (hardware-accelerated via PPA)
// - ppa_fill_rect() for rectangle fills (hardware-accelerated via PPA)
// These legacy functions remain for compatibility but use CPU fallback only
void fb_fill(uint16_t *fb, int width, int height, uint16_t color);  // DEPRECATED - use PPA Fill
void fb_rect(uint16_t *fb, int width, int height, int x, int y, int w, int h, uint16_t color);  // DEPRECATED - use PPA Fill
