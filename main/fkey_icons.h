#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize F-key icons by loading PNGs from internal flash
 *
 * Loads F1-F6 icons from /int/icons/keyboard/f1.png through f6.png
 * Icons are converted to RGB565 format for direct framebuffer drawing
 */
void fkey_icons_init(void);

/**
 * @brief Free all loaded F-key icons
 */
void fkey_icons_free(void);

/**
 * @brief Check if a specific F-key icon is available
 *
 * @param fkey_num F-key number (1-6)
 * @return true if icon is loaded and available
 */
bool fkey_icon_available(int fkey_num);

/**
 * @brief Get icon dimensions
 *
 * @param fkey_num F-key number (1-6)
 * @param width Output: icon width in pixels (NULL to skip)
 * @param height Output: icon height in pixels (NULL to skip)
 * @return true if icon exists
 */
bool fkey_icon_get_size(int fkey_num, int *width, int *height);

/**
 * @brief Draw F-key icon to framebuffer
 *
 * @param fb Framebuffer (RGB565 format)
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param x X position to draw at
 * @param y Y position to draw at
 * @param fkey_num F-key number (1-6)
 * @param scale Scale factor (1 = original size)
 */
void fkey_icon_draw(uint16_t *fb, int fb_width, int fb_height,
                    int x, int y, int fkey_num, int scale);
