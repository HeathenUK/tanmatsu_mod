/**
 * @file ui_file_browser.c
 * @brief File browser UI rendering implementation
 */

#include "ui_file_browser.h"
#include "ui_theme.h"
#include "ui_primitives.h"
#include "ui_icons.h"
#include "ui_hints.h"
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
    const int entry_scale = 1;
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

    // Item count (right-aligned)
    char count_text[24];
    snprintf(count_text, sizeof(count_text), "%d items", browser->count);
    int count_w = strlen(count_text) * FONT_WIDTH;
    int count_x = fb_width - MARGIN_RIGHT - count_w;
    font_draw_string_scaled(fb, fb_width, fb_height,
                            count_x, path_y,
                            THEME_TEXT_MUTED, 1, count_text);

    // File list area
    int file_list_y = path_y + FONT_HEIGHT + 8;
    if (browser->search_active) {
        const int search_scale = 2;
        char search_text[80];
        snprintf(search_text, sizeof(search_text), "Search: %.30s", browser->search_query);
        font_draw_string_scaled(fb, fb_width, fb_height,
                                MARGIN_LEFT, file_list_y,
                                THEME_TEXT_MUTED, search_scale, search_text);
        file_list_y += FONT_HEIGHT * search_scale + 6;
    }
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

        // Draw filename + metadata
        file_entry_t *entry = &browser->files[i];
        file_browser_fill_cached_meta(browser, entry);

        int list_right = fb_width - MARGIN_RIGHT - 8;
        int max_chars = (list_right - text_offset_x) / (FONT_WIDTH * entry_scale);
        char name[128];
        char meta[128];
        name[0] = '\0';
        meta[0] = '\0';

        strncat(name, entry->filename, sizeof(name) - 1);
        if (entry->meta_title[0] != '\0') {
            strncat(meta, entry->meta_title, sizeof(meta) - 1);
            if (entry->meta_game[0] != '\0') {
                strncat(meta, " - ", sizeof(meta) - strlen(meta) - 1);
                strncat(meta, entry->meta_game, sizeof(meta) - strlen(meta) - 1);
            }
        }

        if (max_chars > 0 && (int)strlen(meta) > max_chars) {
            if (max_chars > 3) {
                meta[max_chars - 3] = '.';
                meta[max_chars - 2] = '.';
                meta[max_chars - 1] = '.';
                meta[max_chars] = '\0';
            } else {
                meta[max_chars] = '\0';
            }
        }

        int meta_w = (int)strlen(meta) * FONT_WIDTH * entry_scale;
        const int gap_px = 8;
        int name_max_px = list_right - text_offset_x - (meta_w ? (meta_w + gap_px) : 0);
        int name_max_chars = name_max_px / (FONT_WIDTH * entry_scale);
        if (name_max_chars < 0) name_max_chars = 0;
        if (name_max_chars > 0 && (int)strlen(name) > name_max_chars) {
            if (name_max_chars > 3) {
                name[name_max_chars - 3] = '.';
                name[name_max_chars - 2] = '.';
                name[name_max_chars - 1] = '.';
                name[name_max_chars] = '\0';
            } else {
                name[name_max_chars] = '\0';
            }
        }

        int text_y = row_y + (row_height - FONT_HEIGHT * entry_scale) / 2;
        uint16_t name_color = (i == browser->selected_index) ? THEME_TEXT_SECONDARY : THEME_TEXT_MUTED;
        uint16_t meta_color = (i == browser->selected_index) ? THEME_TEXT_PRIMARY : THEME_TEXT_SECONDARY;
        font_draw_string_scaled(fb, fb_width, fb_height,
                                text_offset_x, text_y, name_color, entry_scale, name);
        if (meta[0] != '\0') {
            int meta_x = list_right - meta_w;
            if (meta_x < text_offset_x + gap_px) {
                meta_x = text_offset_x + gap_px;
            }
            font_draw_string_scaled(fb, fb_width, fb_height,
                                    meta_x, text_y, meta_color, entry_scale, meta);
        }
    }

    // Scroll indicator
    if (browser->count > 0) {
        int track_x = fb_width - MARGIN_RIGHT + 2;
        int track_w = 4;
        int track_y = file_list_y;
        int track_h = end_idx > start_idx ? (end_idx - start_idx) * row_height : row_height;
        hw_accel_fill_rect(fb, fb_width, fb_height,
                           track_x, track_y, track_w, track_h, THEME_BG_SECONDARY);
        int thumb_h = (browser->count > 0) ? (track_h * (end_idx - start_idx) / browser->count) : track_h;
        if (thumb_h < 6) thumb_h = 6;
        int max_pos = (browser->count > 1) ? (browser->count - 1) : 1;
        int thumb_y = track_y + (track_h - thumb_h) * browser->selected_index / max_pos;
        hw_accel_fill_rect(fb, fb_width, fb_height,
                           track_x, thumb_y, track_w, thumb_h, THEME_TEXT_MUTED);
    }

    // Footer hint bar
    const int hint_bar_height = 20;
    int hint_bar_y = fb_height - MARGIN_BOTTOM - hint_bar_height;
    const char *sort_label = (browser->sort_mode == 0) ? "Sort: A→Z" : "Sort: Z→A";
    const char *back_label = "Parent directory";
    if (browser->search_active) {
        back_label = "Exit results";
    } else if (strcmp(browser->current_path, "/sdcard") == 0) {
        back_label = "Exit to Launcher";
    }
    const ui_hint_item_t hints[] = {
        {5, "Search"},
        {2, sort_label},
        {6, back_label},
    };
    ui_draw_hint_bar(fb, fb_width, fb_height, hint_bar_y, hint_bar_height,
                     THEME_BG_PRIMARY, THEME_TEXT_MUTED, hints,
                     (int)(sizeof(hints) / sizeof(hints[0])), 20);

    if (browser->search_cache_building) {
        const char *indexing_text = "Indexing...";
        int text_w = (int)strlen(indexing_text) * FONT_WIDTH;
        int text_x = fb_width - MARGIN_RIGHT - text_w;
        int text_y = hint_bar_y + (hint_bar_height - FONT_HEIGHT) / 2;
        font_draw_string_scaled(fb, fb_width, fb_height,
                                text_x, text_y, THEME_TEXT_MUTED, 1, indexing_text);
    }

}

void ui_draw_browser_volume_osd(uint16_t *fb, int fb_width, int fb_height, float volume) {
    int vol_percent = (int)(volume * 100.0f + 0.5f);
    if (vol_percent < 0) vol_percent = 0;
    if (vol_percent > 100) vol_percent = 100;

    char vol_text[24];
    snprintf(vol_text, sizeof(vol_text), "Vol: %d%%", vol_percent);
    int text_w = (int)strlen(vol_text) * FONT_WIDTH * 2;
    int box_w = text_w + 16;
    int box_h = FONT_HEIGHT * 2 + 10;
    int box_x = fb_width - MARGIN_RIGHT - box_w;
    int box_y = MARGIN_TOP + 6;

    hw_accel_fill_rect(fb, fb_width, fb_height,
                       box_x, box_y, box_w, box_h, THEME_BG_SECONDARY);
    font_draw_string_scaled(fb, fb_width, fb_height,
                            box_x + 8, box_y + 5, THEME_TEXT_PRIMARY, 2, vol_text);
}
