/**
 * @file ui_file_browser.c
 * @brief File browser UI rendering implementation
 */

#include "ui_file_browser.h"
#include "ui_theme.h"
#include "ui_primitives.h"
#include "ui_icons.h"
#include "simple_font.h"
#include "graphics/hw_accel.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Screen margins
#define MARGIN_LEFT   10
#define MARGIN_RIGHT  10
#define MARGIN_TOP    5
#define MARGIN_BOTTOM 5

// Content area
#define CONTENT_WIDTH(fb_width)  ((fb_width) - MARGIN_LEFT - MARGIN_RIGHT)
#define CONTENT_HEIGHT(fb_height) ((fb_height) - MARGIN_TOP - MARGIN_BOTTOM)

void ui_format_channel_string(char *ch_str, size_t ch_str_size,
                               unsigned char note, unsigned char ins,
                               unsigned char fxt, unsigned char fxp,
                               bool use_compact, const char *note_names[12]) {
    char note_str[4];
    if (note > 0 && note <= 96) {
        int note_idx = (note - 1) % 12;
        int octave = (note - 1) / 12;
        if (note_idx < 0) note_idx = 0;
        if (note_idx > 11) note_idx = 11;
        if (octave < 0) octave = 0;
        if (octave > 9) octave = 9;
        const char *note_name = note_names[note_idx];
        if (use_compact) {
            // Compact: "C4" or "C#4"
            if (note_name[1] == '#') {
                snprintf(note_str, sizeof(note_str), "%c#%d", note_name[0], octave);
            } else {
                snprintf(note_str, sizeof(note_str), "%c%d", note_name[0], octave);
            }
        } else {
            // Full: "C-4" or "C#4"
            snprintf(note_str, sizeof(note_str), "%s%d", note_name, octave);
        }
    } else {
        strcpy(note_str, use_compact ? "--" : "---");
    }

    // Format instrument: "--" if no instrument, else hex value
    char ins_str[3];
    if (ins == 0) {
        strcpy(ins_str, "--");
    } else {
        snprintf(ins_str, sizeof(ins_str), "%02X", ins);
    }

    if (fxt == 0 && fxp == 0) {
        snprintf(ch_str, ch_str_size, "%s %s ----", note_str, ins_str);
    } else {
        snprintf(ch_str, ch_str_size, "%s %s %02X%02X", note_str, ins_str, fxt, fxp);
    }
}

void ui_draw_file_browser(uint16_t *fb, int fb_width, int fb_height, file_browser_t *browser) {
    const int font_scale = 2;
    const int row_height = THEME_ROW_HEIGHT_MD;  // 32px per row
    const int header_height = THEME_HEADER_HEIGHT;  // 40px header
    const int icon_size = UI_ICON_WIDTH;
    const int icon_padding = THEME_ICON_PADDING;
    const int text_offset_x = MARGIN_LEFT + icon_size + icon_padding * 2;

    // Clear background
    hw_accel_fill_framebuffer(fb, fb_width, fb_height, THEME_BG_PRIMARY);

    // Draw header with gradient
    ui_draw_vgradient(fb, fb_width, fb_height,
                      0, MARGIN_TOP, fb_width, header_height,
                      THEME_BG_HEADER, THEME_BG_PRIMARY);

    // Header title (centered)
    const char *title = "Trackmatsu";
    int title_width = strlen(title) * FONT_WIDTH * font_scale;
    int title_x = (fb_width - title_width) / 2;
    font_draw_string_scaled(fb, fb_width, fb_height,
                            title_x, MARGIN_TOP + 4,
                            THEME_TEXT_PRIMARY, font_scale, title);

    // Path display (below header)
    int path_y = MARGIN_TOP + header_height + 2;
    char path_text[80];
    int path_len = snprintf(path_text, sizeof(path_text), "%s", browser->current_path);
    if (path_len >= (int)sizeof(path_text)) {
        path_text[sizeof(path_text) - 1] = '\0';
    }
    font_draw_string_scaled(fb, fb_width, fb_height,
                            MARGIN_LEFT, path_y,
                            THEME_TEXT_SECONDARY, 1, path_text);

    // File list area
    int file_list_y = path_y + FONT_HEIGHT + 8;
    int visible_rows = (fb_height - file_list_y - MARGIN_BOTTOM - row_height) / row_height;
    if (visible_rows > 12) visible_rows = 12;

    // Calculate visible range (center selection when possible)
    int half_visible = visible_rows / 2;
    int start_idx = browser->selected_index - half_visible;
    if (start_idx < 0) start_idx = 0;
    int end_idx = start_idx + visible_rows;
    if (end_idx > browser->count) {
        end_idx = browser->count;
        start_idx = end_idx - visible_rows;
        if (start_idx < 0) start_idx = 0;
    }

    // Draw alternating row backgrounds
    for (int i = start_idx; i < end_idx; i++) {
        int row_idx = i - start_idx;
        int row_y = file_list_y + row_idx * row_height;
        uint16_t row_bg = (row_idx % 2 == 0) ? THEME_BG_PRIMARY : THEME_BG_SECONDARY;
        hw_accel_fill_rect(fb, fb_width, fb_height,
                          MARGIN_LEFT, row_y, CONTENT_WIDTH(fb_width), row_height, row_bg);
    }

    // Draw icons and text (selected item uses white text)
    for (int i = start_idx; i < end_idx; i++) {
        int row_idx = i - start_idx;
        int row_y = file_list_y + row_idx * row_height;

        // Determine icon and color based on file type
        bool is_parent = (strcmp(browser->files[i].filename, "..") == 0);
        bool is_dir = browser->files[i].is_dir;
        const char *ext = strrchr(browser->files[i].filename, '.');
        ui_icon_id_t icon_id = ui_get_file_icon_id(ext, is_dir, is_parent);

        // Icon color based on type
        uint16_t icon_color;
        switch (icon_id) {
            case UI_ICON_ID_FOLDER: icon_color = THEME_ACCENT_5; break;  // Magenta
            case UI_ICON_ID_PARENT: icon_color = THEME_ACCENT_6; break;  // Cyan
            case UI_ICON_ID_MOD:    icon_color = THEME_ACCENT_1; break;  // Red
            case UI_ICON_ID_XM:     icon_color = THEME_ACCENT_2; break;  // Green
            case UI_ICON_ID_S3M:    icon_color = THEME_ACCENT_3; break;  // Blue
            case UI_ICON_ID_IT:     icon_color = THEME_ACCENT_4; break;  // Yellow
            default:                icon_color = THEME_TEXT_SECONDARY; break;
        }

        // Draw icon
        int icon_y = row_y + (row_height - icon_size) / 2;
        const uint8_t *icon_data = ui_get_icon(icon_id);
        if (icon_data) {
            ui_draw_icon(fb, fb_width, fb_height,
                         MARGIN_LEFT + icon_padding, icon_y,
                         icon_data, UI_ICON_WIDTH, UI_ICON_HEIGHT,
                         icon_color);
        }

        // Draw filename
        char name[64];
        strncpy(name, browser->files[i].filename, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';

        int text_y = row_y + (row_height - FONT_HEIGHT * font_scale) / 2;
        uint16_t text_color = (i == browser->selected_index) ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY;
        font_draw_string_scaled(fb, fb_width, fb_height,
                                text_offset_x, text_y, text_color, font_scale, name);
    }

    // Footer hint bar (same style as playback views)
    int hint_bar_height = 20;
    int hint_bar_y = fb_height - MARGIN_BOTTOM - hint_bar_height;
    hw_accel_fill_rect(fb, fb_width, fb_height,
                      0, hint_bar_y, fb_width, hint_bar_height, THEME_BG_SECONDARY);

    // Calculate total width for centering
    // ↑↓ Nav | ⏎ Select | ← Back
    int gap = 20;
    int total_width = 0;
    total_width += FONT_WIDTH * 6 + gap;   // "↑↓ Nav" (6 chars)
    total_width += FONT_WIDTH * 8 + gap;   // "⏎ Select" (8 chars)
    total_width += FONT_WIDTH * 6;          // "← Back" (6 chars)

    int hint_x = (fb_width - total_width) / 2;
    int text_y = hint_bar_y + (hint_bar_height - FONT_HEIGHT) / 2 + 1;

    // ↑↓ Nav (chars 0x80, 0x81)
    font_draw_string_scaled(fb, fb_width, fb_height, hint_x, text_y, THEME_TEXT_MUTED, 1, "\x80\x81 Nav");
    hint_x += FONT_WIDTH * 6 + gap;

    // ⏎ Select (char 0x84)
    font_draw_string_scaled(fb, fb_width, fb_height, hint_x, text_y, THEME_TEXT_MUTED, 1, "\x84 Select");
    hint_x += FONT_WIDTH * 8 + gap;

    // ← Back (char 0x82)
    font_draw_string_scaled(fb, fb_width, fb_height, hint_x, text_y, THEME_TEXT_MUTED, 1, "\x82 Back");
}
