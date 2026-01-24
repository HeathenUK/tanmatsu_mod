/**
 * @file ui_file_browser.h
 * @brief File browser UI rendering
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "file_browser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Draw the file browser interface
 *
 * Renders the file browser with:
 * - Header with title
 * - Current path display
 * - File list with icons and colors
 * - Footer with navigation hints
 *
 * @param fb Pointer to framebuffer (800x480 RGB565)
 * @param fb_width Framebuffer width
 * @param fb_height Framebuffer height
 * @param browser Pointer to file browser state
 */
void ui_draw_file_browser(uint16_t *fb, int fb_width, int fb_height, file_browser_t *browser);
void ui_draw_browser_volume_osd(uint16_t *fb, int fb_width, int fb_height, float volume);

/**
 * @brief Format a channel's event data as a string
 *
 * Formats tracker channel data (note, instrument, effect) for display.
 * Supports both full and compact formats.
 *
 * @param ch_str Output buffer for formatted string
 * @param ch_str_size Size of output buffer
 * @param note MIDI note number (1-96, or 0 for no note)
 * @param ins Instrument number (0-255)
 * @param fxt Effect type (0-255)
 * @param fxp Effect parameter (0-255)
 * @param use_compact Use compact format (e.g., "C4" vs "C-4")
 * @param note_names Array of 12 note name strings
 */
void ui_format_channel_string(char *ch_str, size_t ch_str_size,
                               unsigned char note, unsigned char ins,
                               unsigned char fxt, unsigned char fxp,
                               bool use_compact, const char *note_names[12]);

#ifdef __cplusplus
}
#endif
