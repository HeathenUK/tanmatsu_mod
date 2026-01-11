#pragma once

#include <stdint.h>
#include <stddef.h>

// Simple 6x8 bitmap font (ASCII 32-126)
// Each character is 6 pixels wide, 8 pixels tall
// Stored as 6 bytes per character (one byte per column, LSB is top pixel)
#define FONT_WIDTH 6
#define FONT_HEIGHT 8

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

// Render a string with scaling (scale factor: 1=6x8, 2=12x16, etc.)
void font_draw_string_scaled(uint16_t *fb, int width, int height, int x, int y, uint16_t color, int scale, const char *text);

// Fill framebuffer with a solid color
void fb_fill(uint16_t *fb, int width, int height, uint16_t color);

// Draw a filled rectangle
void fb_rect(uint16_t *fb, int width, int height, int x, int y, int w, int h, uint16_t color);
