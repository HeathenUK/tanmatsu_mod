#include "file_browser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "miniz.h"
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

static const char *TAG = "file_browser";
static const int FILE_BROWSER_INIT_CAP = 64;
static int g_sort_mode = 0;

static int compare_entries(const void *a, const void *b) {
    const file_entry_t *entry_a = (const file_entry_t *)a;
    const file_entry_t *entry_b = (const file_entry_t *)b;
    
    // Directories first
    if (entry_a->is_dir && !entry_b->is_dir) return -1;
    if (!entry_a->is_dir && entry_b->is_dir) return 1;
    
    // Then alphabetical
    int cmp = strcasecmp(entry_a->filename, entry_b->filename);
    return (g_sort_mode == 1) ? -cmp : cmp;
}

static bool file_browser_ensure_capacity(file_entry_t **entries, int *capacity, int needed) {
    if (needed <= *capacity) return true;
    int new_cap = *capacity > 0 ? *capacity : FILE_BROWSER_INIT_CAP;
    while (new_cap < needed) {
        new_cap *= 2;
        if (new_cap < 0) {
            return false;
        }
    }

    file_entry_t *new_entries = (file_entry_t *)heap_caps_malloc(sizeof(file_entry_t) * new_cap, MALLOC_CAP_SPIRAM);
    if (!new_entries) {
        return false;
    }
    if (*entries && *capacity > 0) {
        memcpy(new_entries, *entries, sizeof(file_entry_t) * (*capacity));
        heap_caps_free(*entries);
    }
    *entries = new_entries;
    *capacity = new_cap;
    return true;
}

static bool file_browser_match_name(const char *name, const char *query) {
    if (!query || !query[0]) return true;
    if (!name) return false;
    size_t qlen = strlen(query);
    size_t nlen = strlen(name);
    if (qlen > nlen) return false;
    for (size_t i = 0; i <= nlen - qlen; i++) {
        size_t j = 0;
        for (; j < qlen; j++) {
            char a = name[i + j];
            char b = query[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) break;
        }
        if (j == qlen) return true;
    }
    return false;
}

static bool file_browser_is_music_file(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".mod") == 0 ||
            strcasecmp(ext, ".xm") == 0 ||
            strcasecmp(ext, ".s3m") == 0 ||
            strcasecmp(ext, ".it") == 0 ||
            strcasecmp(ext, ".vgm") == 0 ||
            strcasecmp(ext, ".vgz") == 0);
}

static void file_browser_add_result(file_browser_t *browser,
                                    const char *display_name,
                                    const char *full_path,
                                    bool is_dir) {
    if (!file_browser_ensure_capacity(&browser->files, &browser->capacity, browser->count + 1)) {
        return;
    }
    file_entry_t *file = &browser->files[browser->count];
    strncpy(file->filename, display_name, sizeof(file->filename) - 1);
    file->filename[sizeof(file->filename) - 1] = '\0';
    strncpy(file->full_path, full_path, sizeof(file->full_path) - 1);
    file->full_path[sizeof(file->full_path) - 1] = '\0';
    file->meta_title[0] = '\0';
    file->meta_game[0] = '\0';
    file->is_dir = is_dir;
    browser->count++;
}

static void file_browser_trim_text(char *text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\0')) {
        text[len - 1] = '\0';
        len--;
    }
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') {
        start++;
    }
    if (start > 0) {
        memmove(text, text + start, strlen(text + start) + 1);
    }
}

static bool file_browser_extract_tracker_title(const char *path, char *out_title, size_t out_len) {
    if (!path || !out_title || out_len == 0) return false;
    out_title[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    const char *ext = strrchr(path, '.');
    if (!ext) {
        fclose(f);
        return false;
    }

    bool ok = false;
    if (strcasecmp(ext, ".mod") == 0) {
        char buf[20] = {0};
        if (fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
            memcpy(out_title, buf, sizeof(buf));
            out_title[sizeof(buf)] = '\0';
            file_browser_trim_text(out_title);
            ok = out_title[0] != '\0';
        }
    } else if (strcasecmp(ext, ".xm") == 0) {
        char header[17] = {0};
        if (fread(header, 1, sizeof(header), f) == sizeof(header) &&
            memcmp(header, "Extended Module: ", 17) == 0) {
            char buf[20] = {0};
            if (fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
                memcpy(out_title, buf, sizeof(buf));
                out_title[sizeof(buf)] = '\0';
                file_browser_trim_text(out_title);
                ok = out_title[0] != '\0';
            }
        }
    } else if (strcasecmp(ext, ".s3m") == 0) {
        char title[28] = {0};
        if (fread(title, 1, sizeof(title), f) == sizeof(title)) {
            fseek(f, 0x2C, SEEK_SET);
            char sig[4] = {0};
            if (fread(sig, 1, sizeof(sig), f) == sizeof(sig) && memcmp(sig, "SCRM", 4) == 0) {
                memcpy(out_title, title, sizeof(title));
                out_title[sizeof(title)] = '\0';
                file_browser_trim_text(out_title);
                ok = out_title[0] != '\0';
            }
        }
    } else if (strcasecmp(ext, ".it") == 0) {
        char sig[4] = {0};
        if (fread(sig, 1, sizeof(sig), f) == sizeof(sig) && memcmp(sig, "IMPM", 4) == 0) {
            char buf[26] = {0};
            if (fread(buf, 1, sizeof(buf), f) == sizeof(buf)) {
                memcpy(out_title, buf, sizeof(buf));
                out_title[sizeof(buf)] = '\0';
                file_browser_trim_text(out_title);
                ok = out_title[0] != '\0';
            }
        }
    }

    fclose(f);
    return ok;
}

static uint32_t file_browser_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool file_browser_is_gzip(const uint8_t *data, size_t size) {
    return size >= 2 && data[0] == 0x1F && data[1] == 0x8B;
}

static uint8_t *file_browser_decompress_vgz(const uint8_t *data, size_t size, size_t *out_size) {
    if (!data || size < 18 || !out_size) return NULL;

    size_t header_size = 10;
    uint8_t flags = data[3];

    if (flags & 0x04) {
        if (header_size + 2 > size) return NULL;
        uint16_t xlen = (uint16_t)(data[header_size] | (data[header_size + 1] << 8));
        header_size += 2 + xlen;
    }
    if (flags & 0x08) {
        while (header_size < size && data[header_size] != 0) header_size++;
        header_size++;
    }
    if (flags & 0x10) {
        while (header_size < size && data[header_size] != 0) header_size++;
        header_size++;
    }
    if (flags & 0x02) {
        header_size += 2;
    }

    if (header_size >= size - 8) return NULL;

    size_t orig_size = file_browser_read_le32(data + size - 4);
    if (orig_size == 0 || orig_size > 32 * 1024 * 1024) {
        return NULL;
    }

    uint8_t *out = (uint8_t *)heap_caps_malloc(orig_size, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;

    tinfl_decompressor *decomp = (tinfl_decompressor *)heap_caps_malloc(
        sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    if (!decomp) {
        heap_caps_free(out);
        return NULL;
    }
    tinfl_init(decomp);

    const mz_uint8 *in_ptr = (const mz_uint8 *)(data + header_size);
    size_t in_remaining = size - header_size - 8;
    mz_uint8 *out_ptr = (mz_uint8 *)out;
    size_t out_remaining = orig_size;
    size_t total_out = 0;

    tinfl_status status;
    do {
        size_t in_bytes = in_remaining;
        size_t out_bytes = out_remaining;

        mz_uint32 flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (in_remaining > 0) {
            flags |= TINFL_FLAG_HAS_MORE_INPUT;
        }

        status = tinfl_decompress(decomp, in_ptr, &in_bytes,
                                  (mz_uint8 *)out, out_ptr, &out_bytes, flags);

        in_ptr += in_bytes;
        in_remaining -= in_bytes;
        out_ptr += out_bytes;
        out_remaining -= out_bytes;
        total_out += out_bytes;
    } while (status == TINFL_STATUS_HAS_MORE_OUTPUT ||
             (status == TINFL_STATUS_NEEDS_MORE_INPUT && in_remaining > 0));

    heap_caps_free(decomp);

    if (status != TINFL_STATUS_DONE) {
        heap_caps_free(out);
        return NULL;
    }

    *out_size = total_out;
    return out;
}

static size_t file_browser_utf16le_to_utf8(const uint8_t *in, size_t in_bytes,
                                           char *out, size_t out_len) {
    if (!in || in_bytes < 2) return 0;
    size_t out_pos = 0;
    size_t in_pos = 0;
    while (in_pos + 1 < in_bytes) {
        uint16_t cp = (uint16_t)(in[in_pos] | (in[in_pos + 1] << 8));
        if (cp == 0x0000) {
            in_pos += 2;
            break;
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = '?';
        }
        if (cp <= 0x7F) {
            if (out && out_pos + 1 < out_len) out[out_pos++] = (char)cp;
        } else if (cp <= 0x7FF) {
            if (out && out_pos + 2 < out_len) {
                out[out_pos++] = (char)(0xC0 | (cp >> 6));
                out[out_pos++] = (char)(0x80 | (cp & 0x3F));
            }
        } else {
            if (out && out_pos + 3 < out_len) {
                out[out_pos++] = (char)(0xE0 | (cp >> 12));
                out[out_pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[out_pos++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        in_pos += 2;
    }
    if (out && out_len > 0) {
        out[out_pos < out_len ? out_pos : (out_len - 1)] = '\0';
    }
    return in_pos;
}

static bool file_browser_parse_gd3(const uint8_t *data, size_t size,
                                   char *out_title, size_t out_title_len,
                                   char *out_game, size_t out_game_len) {
    if (!data || size < 0x40 || !out_title || !out_game) return false;
    out_title[0] = '\0';
    out_game[0] = '\0';

    if (memcmp(data, "Vgm ", 4) != 0) return false;
    uint32_t gd3_offset = file_browser_read_le32(data + 0x14);
    if (gd3_offset == 0) return false;
    size_t gd3_pos = 0x14 + (size_t)gd3_offset;
    if (gd3_pos + 12 > size) return false;
    if (memcmp(data + gd3_pos, "Gd3 ", 4) != 0) return false;

    uint32_t gd3_length = file_browser_read_le32(data + gd3_pos + 8);
    size_t gd3_data_pos = gd3_pos + 12;
    if (gd3_data_pos + gd3_length > size) return false;

    const uint8_t *p = data + gd3_data_pos;
    size_t remaining = gd3_length;

    char title_en[MAX_FILENAME_LEN] = {0};
    char title_jp[MAX_FILENAME_LEN] = {0};
    char game_en[MAX_FILENAME_LEN] = {0};
    char game_jp[MAX_FILENAME_LEN] = {0};

    size_t consumed = file_browser_utf16le_to_utf8(p, remaining, title_en, sizeof(title_en));
    if (consumed == 0 || consumed > remaining) return false;
    p += consumed; remaining -= consumed;
    consumed = file_browser_utf16le_to_utf8(p, remaining, title_jp, sizeof(title_jp));
    if (consumed == 0 || consumed > remaining) return false;
    p += consumed; remaining -= consumed;

    consumed = file_browser_utf16le_to_utf8(p, remaining, game_en, sizeof(game_en));
    if (consumed == 0 || consumed > remaining) return false;
    p += consumed; remaining -= consumed;
    consumed = file_browser_utf16le_to_utf8(p, remaining, game_jp, sizeof(game_jp));
    if (consumed == 0 || consumed > remaining) return false;

    if (title_en[0]) {
        strncpy(out_title, title_en, out_title_len - 1);
        out_title[out_title_len - 1] = '\0';
    } else if (title_jp[0]) {
        strncpy(out_title, title_jp, out_title_len - 1);
        out_title[out_title_len - 1] = '\0';
    }

    if (game_en[0]) {
        strncpy(out_game, game_en, out_game_len - 1);
        out_game[out_game_len - 1] = '\0';
    } else if (game_jp[0]) {
        strncpy(out_game, game_jp, out_game_len - 1);
        out_game[out_game_len - 1] = '\0';
    }

    file_browser_trim_text(out_title);
    file_browser_trim_text(out_game);
    return out_title[0] != '\0' || out_game[0] != '\0';
}

static bool file_browser_extract_vgm_tags(const char *path,
                                          char *out_title,
                                          size_t out_title_len,
                                          char *out_game,
                                          size_t out_game_len) {
    if (!path || !out_title || !out_game) return false;
    out_title[0] = '\0';
    out_game[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    uint8_t *data = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!data) {
        fclose(f);
        return false;
    }
    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        heap_caps_free(data);
        return false;
    }

    bool ok = false;
    if (file_browser_is_gzip(data, (size_t)size)) {
        size_t vgm_size = 0;
        uint8_t *vgm_data = file_browser_decompress_vgz(data, (size_t)size, &vgm_size);
        if (vgm_data) {
            ok = file_browser_parse_gd3(vgm_data, vgm_size,
                                        out_title, out_title_len,
                                        out_game, out_game_len);
            heap_caps_free(vgm_data);
        }
    } else {
        ok = file_browser_parse_gd3(data, (size_t)size,
                                    out_title, out_title_len,
                                    out_game, out_game_len);
    }

    heap_caps_free(data);
    return ok;
}

static void file_browser_scan_recursive(file_browser_t *browser,
                                        const char *base_path,
                                        const char *rel_prefix,
                                        const char *query) {
    DIR *dir = opendir(base_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' && strcmp(entry->d_name, "..") != 0) {
            continue;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[MAX_PATH_LEN];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name) >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            char next_rel[MAX_FILENAME_LEN];
            if (rel_prefix && rel_prefix[0]) {
                if (snprintf(next_rel, sizeof(next_rel), "%s/%s", rel_prefix, entry->d_name) >= (int)sizeof(next_rel)) {
                    continue;
                }
            } else {
                if (snprintf(next_rel, sizeof(next_rel), "%s", entry->d_name) >= (int)sizeof(next_rel)) {
                    continue;
                }
            }
            file_browser_scan_recursive(browser, full_path, next_rel, query);
        } else {
            if (!file_browser_is_music_file(entry->d_name)) {
                continue;
            }
            char display_name[MAX_FILENAME_LEN];
            if (rel_prefix && rel_prefix[0]) {
                if (snprintf(display_name, sizeof(display_name), "%s/%s", rel_prefix, entry->d_name) >= (int)sizeof(display_name)) {
                    continue;
                }
            } else {
                if (snprintf(display_name, sizeof(display_name), "%s", entry->d_name) >= (int)sizeof(display_name)) {
                    continue;
                }
            }
            if (query && !file_browser_match_name(display_name, query)) {
                continue;
            }
            file_browser_add_result(browser, display_name, full_path, false);
        }
    }
    closedir(dir);
}

static void file_browser_scan_recursive_cache(file_entry_t **entries,
                                              int *count,
                                              int *capacity,
                                              const char *base_path,
                                              const char *rel_prefix) {
    DIR *dir = opendir(base_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' && strcmp(entry->d_name, "..") != 0) {
            continue;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[MAX_PATH_LEN];
        if (snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name) >= (int)sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            char next_rel[MAX_FILENAME_LEN];
            if (rel_prefix && rel_prefix[0]) {
                if (snprintf(next_rel, sizeof(next_rel), "%s/%s", rel_prefix, entry->d_name) >= (int)sizeof(next_rel)) {
                    continue;
                }
            } else {
                if (snprintf(next_rel, sizeof(next_rel), "%s", entry->d_name) >= (int)sizeof(next_rel)) {
                    continue;
                }
            }
            file_browser_scan_recursive_cache(entries, count, capacity, full_path, next_rel);
        } else {
            if (!file_browser_is_music_file(entry->d_name)) {
                continue;
            }
            char display_name[MAX_FILENAME_LEN];
            if (rel_prefix && rel_prefix[0]) {
                if (snprintf(display_name, sizeof(display_name), "%s/%s", rel_prefix, entry->d_name) >= (int)sizeof(display_name)) {
                    continue;
                }
            } else {
                if (snprintf(display_name, sizeof(display_name), "%s", entry->d_name) >= (int)sizeof(display_name)) {
                    continue;
                }
            }
            if (!file_browser_ensure_capacity(entries, capacity, *count + 1)) {
                continue;
            }
            file_entry_t *file = &(*entries)[*count];
            strncpy(file->filename, display_name, sizeof(file->filename) - 1);
            file->filename[sizeof(file->filename) - 1] = '\0';
            strncpy(file->full_path, full_path, sizeof(file->full_path) - 1);
            file->full_path[sizeof(file->full_path) - 1] = '\0';
            file->meta_title[0] = '\0';
            file->meta_game[0] = '\0';
            file->is_dir = false;
            (*count)++;
        }
    }
    closedir(dir);
}

typedef struct {
    file_browser_t *browser;
    char root[MAX_PATH_LEN];
} file_browser_cache_task_ctx_t;

static void file_browser_cache_task(void *arg) {
    file_browser_cache_task_ctx_t *ctx = (file_browser_cache_task_ctx_t *)arg;
    if (!ctx || !ctx->browser) {
        vTaskDelete(NULL);
        return;
    }

    file_entry_t *temp_entries = NULL;
    int temp_count = 0;
    int temp_capacity = 0;
    file_browser_scan_recursive_cache(&temp_entries, &temp_count, &temp_capacity, ctx->root, "");

    for (int i = 0; i < temp_count; i++) {
        if (temp_entries[i].is_dir) {
            continue;
        }
        const char *ext = strrchr(temp_entries[i].full_path, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".vgm") == 0 || strcasecmp(ext, ".vgz") == 0) {
            file_browser_extract_vgm_tags(temp_entries[i].full_path,
                                          temp_entries[i].meta_title, sizeof(temp_entries[i].meta_title),
                                          temp_entries[i].meta_game, sizeof(temp_entries[i].meta_game));
        } else {
            file_browser_extract_tracker_title(temp_entries[i].full_path,
                                               temp_entries[i].meta_title, sizeof(temp_entries[i].meta_title));
        }
        if ((i % 8) == 0) {
            vTaskDelay(1);
        }
    }

    SemaphoreHandle_t mutex = (SemaphoreHandle_t)ctx->browser->search_cache_mutex;
    if (mutex) {
        xSemaphoreTake(mutex, portMAX_DELAY);
    }
    if (ctx->browser->search_cache) {
        heap_caps_free(ctx->browser->search_cache);
    }
    ctx->browser->search_cache = temp_entries;
    ctx->browser->search_cache_count = temp_count;
    ctx->browser->search_cache_capacity = temp_capacity;
    strncpy(ctx->browser->search_cache_root, ctx->root, sizeof(ctx->browser->search_cache_root) - 1);
    ctx->browser->search_cache_root[sizeof(ctx->browser->search_cache_root) - 1] = '\0';
    ctx->browser->search_cache_ready = true;
    ctx->browser->search_cache_building = false;
    if (mutex) {
        xSemaphoreGive(mutex);
    }

    heap_caps_free(ctx);
    vTaskDelete(NULL);
}

esp_err_t file_browser_init(file_browser_t *browser, const char *base_path) {
    if (browser == NULL || base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(browser, 0, sizeof(file_browser_t));
    strncpy(browser->current_path, base_path, sizeof(browser->current_path) - 1);
    browser->selected_index = 0;
    browser->search_active = false;
    browser->search_query[0] = '\0';
    browser->last_selected_name[0] = '\0';
    browser->sort_mode = 0;
    browser->capacity = 0;
    browser->all_capacity = 0;
    browser->files = NULL;
    browser->all_files = NULL;
    browser->search_cache = NULL;
    browser->search_cache_count = 0;
    browser->search_cache_capacity = 0;
    browser->search_cache_ready = false;
    browser->search_cache_building = false;
    browser->search_cache_root[0] = '\0';
    browser->search_cache_mutex = xSemaphoreCreateMutex();

    file_browser_build_search_cache_async(browser, base_path);
    
    return file_browser_refresh(browser);
}

esp_err_t file_browser_refresh(file_browser_t *browser) {
    if (browser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    browser->all_count = 0;
    browser->count = 0;
    browser->selected_index = 0;
    
    DIR *dir = opendir(browser->current_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", browser->current_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip hidden files (except ".." which we handle separately)
        if (entry->d_name[0] == '.' && strcmp(entry->d_name, "..") != 0) {
            continue;
        }
        
        // Skip ".." as we handle it separately
        if (strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Build full path
        char full_path[MAX_FILENAME_LEN];
        int len = snprintf(full_path, sizeof(full_path), "%s/%s", browser->current_path, entry->d_name);
        if (len >= (int)sizeof(full_path)) {
            // Path too long, skip this entry
            continue;
        }
        
        // Check if it's a directory
        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Always include directories
                if (!file_browser_ensure_capacity(&browser->all_files, &browser->all_capacity, browser->all_count + 1)) {
                    continue;
                }
                file_entry_t *file = &browser->all_files[browser->all_count];
                strncpy(file->filename, entry->d_name, sizeof(file->filename) - 1);
                file->filename[sizeof(file->filename) - 1] = '\0';
                if (snprintf(file->full_path, sizeof(file->full_path), "%s/%s", browser->current_path, entry->d_name) >= (int)sizeof(file->full_path)) {
                    continue;
                }
                file->meta_title[0] = '\0';
                file->meta_game[0] = '\0';
                file->is_dir = true;
                browser->all_count++;
            } else {
                // For files, include MOD files (.mod, .xm, .s3m, .it) and VGM files (.vgm, .vgz)
                if (file_browser_is_music_file(entry->d_name)) {
                    if (!file_browser_ensure_capacity(&browser->all_files, &browser->all_capacity, browser->all_count + 1)) {
                        continue;
                    }
                    file_entry_t *file = &browser->all_files[browser->all_count];
                    strncpy(file->filename, entry->d_name, sizeof(file->filename) - 1);
                    file->filename[sizeof(file->filename) - 1] = '\0';
                    if (snprintf(file->full_path, sizeof(file->full_path), "%s/%s", browser->current_path, entry->d_name) >= (int)sizeof(file->full_path)) {
                        continue;
                    }
                    file->meta_title[0] = '\0';
                    file->meta_game[0] = '\0';
                    file->is_dir = false;
                    browser->all_count++;
                }
            }
        }
    }
    closedir(dir);

    return file_browser_apply_filter(browser);
}

esp_err_t file_browser_apply_filter(file_browser_t *browser) {
    if (browser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    browser->count = 0;
    browser->selected_index = 0;

    const char *query = (browser->search_active && browser->search_query[0]) ? browser->search_query : NULL;

    if (browser->search_active && query) {
        bool used_cache = false;
        SemaphoreHandle_t mutex = (SemaphoreHandle_t)browser->search_cache_mutex;
        if (browser->search_cache_ready && browser->search_cache && browser->search_cache_root[0] != '\0') {
            size_t root_len = strlen(browser->search_cache_root);
            if (strncmp(browser->current_path, browser->search_cache_root, root_len) == 0) {
                if (mutex) {
                    xSemaphoreTake(mutex, pdMS_TO_TICKS(50));
                }
                size_t cur_len = strlen(browser->current_path);
                for (int i = 0; i < browser->search_cache_count; i++) {
                    const char *full = browser->search_cache[i].full_path;
                    if (strncmp(full, browser->current_path, cur_len) != 0) {
                        continue;
                    }
                    if (full[cur_len] != '/' && full[cur_len] != '\0') {
                        continue;
                    }
                    const char *rel = full + cur_len;
                    if (*rel == '/') rel++;
                    if (!rel[0]) continue;
                    if (!file_browser_match_name(rel, query)) {
                        continue;
                    }
                    file_browser_add_result(browser, rel, full, false);
                    file_entry_t *dest = &browser->files[browser->count - 1];
                    strncpy(dest->meta_title, browser->search_cache[i].meta_title, sizeof(dest->meta_title) - 1);
                    dest->meta_title[sizeof(dest->meta_title) - 1] = '\0';
                    strncpy(dest->meta_game, browser->search_cache[i].meta_game, sizeof(dest->meta_game) - 1);
                    dest->meta_game[sizeof(dest->meta_game) - 1] = '\0';
                }
                if (mutex) {
                    xSemaphoreGive(mutex);
                }
                used_cache = true;
            }
        }
        if (!used_cache) {
            file_browser_scan_recursive(browser, browser->current_path, "", query);
        }
    } else {
        bool is_root = (strcmp(browser->current_path, "/sdcard") == 0);
        if (!is_root) {
            if (file_browser_ensure_capacity(&browser->files, &browser->capacity, browser->count + 1)) {
                file_entry_t *file = &browser->files[browser->count];
                strncpy(file->filename, "..", sizeof(file->filename) - 1);
                file->filename[sizeof(file->filename) - 1] = '\0';
                file->full_path[0] = '\0';
                file->meta_title[0] = '\0';
                file->meta_game[0] = '\0';
                file->is_dir = true;
                browser->count++;
            }
        }

        for (int i = 0; i < browser->all_count; i++) {
            if (!file_browser_match_name(browser->all_files[i].filename, query)) {
                continue;
            }
            if (!file_browser_ensure_capacity(&browser->files, &browser->capacity, browser->count + 1)) {
                continue;
            }
            browser->files[browser->count] = browser->all_files[i];
            browser->count++;
        }

        int start_idx = is_root ? 0 : 1;
        g_sort_mode = browser->sort_mode;
        if (start_idx < browser->count) {
            qsort(browser->files + start_idx, browser->count - start_idx, sizeof(file_entry_t), compare_entries);
        }
    }

    file_browser_restore_selection(browser);

    ESP_LOGI(TAG, "Found %d entries in %s", browser->count, browser->current_path);
    return ESP_OK;
}

void file_browser_set_search_mode(file_browser_t *browser, bool active) {
    if (browser == NULL) return;
    browser->search_active = active;
    if (!active) {
        browser->search_query[0] = '\0';
    } else {
        if (!browser->search_cache_ready ||
            browser->search_cache_root[0] == '\0' ||
            strncmp(browser->current_path, browser->search_cache_root, strlen(browser->search_cache_root)) != 0) {
            file_browser_build_search_cache_async(browser, browser->current_path);
        }
    }
    file_browser_apply_filter(browser);
}

void file_browser_clear_search(file_browser_t *browser) {
    if (browser == NULL) return;
    browser->search_query[0] = '\0';
    file_browser_apply_filter(browser);
}

void file_browser_append_search_char(file_browser_t *browser, char ch) {
    if (browser == NULL) return;
    size_t len = strlen(browser->search_query);
    if (len + 1 >= sizeof(browser->search_query)) return;
    browser->search_query[len] = ch;
    browser->search_query[len + 1] = '\0';
    file_browser_apply_filter(browser);
}

void file_browser_backspace_search(file_browser_t *browser) {
    if (browser == NULL) return;
    size_t len = strlen(browser->search_query);
    if (len == 0) return;
    browser->search_query[len - 1] = '\0';
    file_browser_apply_filter(browser);
}

bool file_browser_jump_to_letter(file_browser_t *browser, char ch) {
    if (browser == NULL || browser->count == 0) return false;
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A'));
    for (int i = 0; i < browser->count; i++) {
        const char *name = browser->files[i].filename;
        if (!name || !name[0]) continue;
        char first = name[0];
        if (first >= 'A' && first <= 'Z') first = (char)(first + ('a' - 'A'));
        if (first == ch) {
            browser->selected_index = i;
            return true;
        }
    }
    return false;
}

void file_browser_free(file_browser_t *browser) {
    if (browser == NULL) return;
    if (browser->files) {
        heap_caps_free(browser->files);
        browser->files = NULL;
        browser->capacity = 0;
    }
    if (browser->all_files) {
        heap_caps_free(browser->all_files);
        browser->all_files = NULL;
        browser->all_capacity = 0;
    }
    if (browser->search_cache) {
        heap_caps_free(browser->search_cache);
        browser->search_cache = NULL;
        browser->search_cache_capacity = 0;
    }
    if (browser->search_cache_mutex) {
        vSemaphoreDelete((SemaphoreHandle_t)browser->search_cache_mutex);
        browser->search_cache_mutex = NULL;
    }
    browser->count = 0;
    browser->all_count = 0;
    browser->search_cache_count = 0;
    browser->search_cache_ready = false;
    browser->search_cache_building = false;
}

void file_browser_remember_selection(file_browser_t *browser) {
    if (browser == NULL || browser->count == 0) return;
    if (browser->selected_index < 0 || browser->selected_index >= browser->count) return;
    const char *name = browser->files[browser->selected_index].filename;
    if (!name) return;
    strncpy(browser->last_selected_name, name, sizeof(browser->last_selected_name) - 1);
    browser->last_selected_name[sizeof(browser->last_selected_name) - 1] = '\0';
}

void file_browser_restore_selection(file_browser_t *browser) {
    if (browser == NULL || browser->count == 0) return;
    if (browser->last_selected_name[0] == '\0') return;
    for (int i = 0; i < browser->count; i++) {
        if (strcmp(browser->files[i].filename, browser->last_selected_name) == 0) {
            browser->selected_index = i;
            break;
        }
    }
}

void file_browser_cycle_sort(file_browser_t *browser) {
    if (browser == NULL) return;
    browser->sort_mode = (browser->sort_mode + 1) % 2;
    file_browser_apply_filter(browser);
}

void file_browser_build_search_cache_async(file_browser_t *browser, const char *root_path) {
    if (browser == NULL || root_path == NULL) return;
    if (browser->search_cache_building) return;
    browser->search_cache_building = true;

    file_browser_cache_task_ctx_t *ctx = heap_caps_malloc(sizeof(file_browser_cache_task_ctx_t), MALLOC_CAP_8BIT);
    if (!ctx) {
        browser->search_cache_building = false;
        return;
    }
    ctx->browser = browser;
    strncpy(ctx->root, root_path, sizeof(ctx->root) - 1);
    ctx->root[sizeof(ctx->root) - 1] = '\0';

    // Indexing is recursive and stack-heavy; give it a larger stack to avoid corruption.
    if (xTaskCreate(file_browser_cache_task, "fb_cache", 32768, ctx, 2, NULL) != pdPASS) {
        heap_caps_free(ctx);
        browser->search_cache_building = false;
    }
}

void file_browser_fill_cached_meta(file_browser_t *browser, file_entry_t *entry) {
    if (!browser || !entry || entry->is_dir) return;
    if (entry->meta_title[0] != '\0' || entry->meta_game[0] != '\0') return;
    if (!browser->search_cache_ready || !browser->search_cache) return;

    SemaphoreHandle_t mutex = (SemaphoreHandle_t)browser->search_cache_mutex;
    if (mutex && xSemaphoreTake(mutex, 0) != pdTRUE) {
        return;
    }

    for (int i = 0; i < browser->search_cache_count; i++) {
        if (strcmp(browser->search_cache[i].full_path, entry->full_path) == 0) {
            strncpy(entry->meta_title, browser->search_cache[i].meta_title, sizeof(entry->meta_title) - 1);
            entry->meta_title[sizeof(entry->meta_title) - 1] = '\0';
            strncpy(entry->meta_game, browser->search_cache[i].meta_game, sizeof(entry->meta_game) - 1);
            entry->meta_game[sizeof(entry->meta_game) - 1] = '\0';
            break;
        }
    }

    if (mutex) {
        xSemaphoreGive(mutex);
    }
}

void file_browser_page_move(file_browser_t *browser, int direction, int page_size) {
    if (browser == NULL || browser->count == 0 || page_size <= 0) return;
    int delta = (direction < 0) ? -page_size : page_size;
    int next = browser->selected_index + delta;
    if (next < 0) next = 0;
    if (next >= browser->count) next = browser->count - 1;
    browser->selected_index = next;
}

void file_browser_up(file_browser_t *browser) {
    if (browser == NULL) return;
    if (browser->selected_index > 0) {
        browser->selected_index--;
    }
}

void file_browser_down(file_browser_t *browser) {
    if (browser == NULL) return;
    if (browser->selected_index < browser->count - 1) {
        browser->selected_index++;
    }
}

esp_err_t file_browser_select(file_browser_t *browser, char *selected_path, size_t path_len) {
    if (browser == NULL || selected_path == NULL || browser->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (browser->selected_index >= browser->count) {
        return ESP_ERR_INVALID_STATE;
    }
    
    file_entry_t *entry = &browser->files[browser->selected_index];

    // Handle ".." specially
    if (entry->is_dir && strcmp(entry->filename, "..") == 0) {
        return file_browser_back(browser);
    }

    if (browser->search_active && entry->full_path[0] != '\0') {
        strncpy(selected_path, entry->full_path, path_len - 1);
        selected_path[path_len - 1] = '\0';
        return ESP_OK;
    }

    // Build full path
    snprintf(selected_path, path_len, "%s/%s", browser->current_path, entry->filename);
    
    if (entry->is_dir) {
        // Enter directory
        strncpy(browser->current_path, selected_path, sizeof(browser->current_path) - 1);
        browser->current_path[sizeof(browser->current_path) - 1] = '\0';
        browser->selected_index = 0;
        browser->search_active = false;
        browser->search_query[0] = '\0';
        return file_browser_refresh(browser);
    }
    
    // File selected
    return ESP_OK;
}

esp_err_t file_browser_back(file_browser_t *browser) {
    if (browser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Find parent directory
    char *last_slash = strrchr(browser->current_path, '/');
    if (last_slash != NULL && last_slash != browser->current_path) {
        *last_slash = '\0';
    } else {
        // Already at root
        return ESP_ERR_INVALID_STATE;
    }
    
    browser->selected_index = 0;
    browser->search_active = false;
    browser->search_query[0] = '\0';
    return file_browser_refresh(browser);
}

const char *file_browser_get_selected(file_browser_t *browser) {
    if (browser == NULL || browser->count == 0 || browser->selected_index >= browser->count) {
        return NULL;
    }
    return browser->files[browser->selected_index].filename;
}
