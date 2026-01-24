/**
 * @file ui_hints.c
 * @brief Hint bar rendering helpers
 */

#include "ui_hints.h"
#include "ui_theme.h"
#include "simple_font.h"
#include "fkey_icons.h"
#include "graphics/hw_accel.h"
#include <string.h>

void ui_draw_hint_bar(uint16_t *fb,
                      int fb_width,
                      int fb_height,
                      int y,
                      int height,
                      uint16_t bg_color,
                      uint16_t text_color,
                      const ui_hint_item_t *items,
                      int item_count,
                      int gap) {
    if (!fb || !items || item_count <= 0) return;

    hw_accel_fill_rect(fb, fb_width, fb_height, 0, y, fb_width, height, bg_color);

    int total_width = 0;
    for (int i = 0; i < item_count; i++) {
        int icon_w = 0;
        if (items[i].fkey > 0 && fkey_icon_available(items[i].fkey)) {
            int icon_h = 0;
            fkey_icon_get_size(items[i].fkey, &icon_w, &icon_h);
            total_width += icon_w;
            if (items[i].label && items[i].label[0]) {
                total_width += 4;
            }
        }
        if (items[i].label && items[i].label[0]) {
            total_width += (int)strlen(items[i].label) * FONT_WIDTH;
        }
        if (i < item_count - 1) {
            total_width += gap;
        }
    }

    int x = (fb_width - total_width) / 2;
    int text_y = y + (height - FONT_HEIGHT) / 2 + 1;

    for (int i = 0; i < item_count; i++) {
        if (items[i].fkey > 0 && fkey_icon_available(items[i].fkey)) {
            int icon_w = 0;
            int icon_h = 0;
            fkey_icon_get_size(items[i].fkey, &icon_w, &icon_h);
            int icon_y = y + (height - icon_h) / 2;
            fkey_icon_draw(fb, fb_width, fb_height, x, icon_y, items[i].fkey, 1);
            x += icon_w;
            if (items[i].label && items[i].label[0]) {
                x += 4;
            }
        }
        if (items[i].label && items[i].label[0]) {
            font_draw_string_scaled(fb, fb_width, fb_height, x, text_y, text_color, 1, items[i].label);
            x += (int)strlen(items[i].label) * FONT_WIDTH;
        }
        if (i < item_count - 1) {
            x += gap;
        }
    }
}
