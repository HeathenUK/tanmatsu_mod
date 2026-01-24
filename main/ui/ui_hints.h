/**
 * @file ui_hints.h
 * @brief Hint bar rendering helpers
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fkey;           // F-key index (1-12). Use 0 for no icon.
    const char *label;  // Optional label text (can be NULL).
} ui_hint_item_t;

void ui_draw_hint_bar(uint16_t *fb,
                      int fb_width,
                      int fb_height,
                      int y,
                      int height,
                      uint16_t bg_color,
                      uint16_t text_color,
                      const ui_hint_item_t *items,
                      int item_count,
                      int gap);

#ifdef __cplusplus
}
#endif
