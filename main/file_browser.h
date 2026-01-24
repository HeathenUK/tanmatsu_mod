#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define MAX_FILENAME_LEN 96   // Display/filename buffer
#define MAX_PATH_LEN 256      // Full path buffer

typedef struct {
    char filename[MAX_FILENAME_LEN];
    char full_path[MAX_PATH_LEN];
    char meta_title[MAX_FILENAME_LEN];
    char meta_game[MAX_FILENAME_LEN];
    bool is_dir;
} file_entry_t;

typedef struct {
    file_entry_t *files;
    int count;
    int capacity;
    int selected_index;
    char current_path[MAX_PATH_LEN];
    file_entry_t *all_files;
    int all_count;
    int all_capacity;
    file_entry_t *search_cache;
    int search_cache_count;
    int search_cache_capacity;
    bool search_cache_ready;
    bool search_cache_building;
    char search_cache_root[MAX_PATH_LEN];
    void *search_cache_mutex;
    bool search_active;
    char search_query[32];
    char last_selected_name[MAX_FILENAME_LEN];
    int sort_mode;
} file_browser_t;

/**
 * @brief Initialize file browser
 * 
 * @param browser Pointer to file browser structure
 * @param base_path Base path to browse (e.g., "/sdcard")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t file_browser_init(file_browser_t *browser, const char *base_path);

/**
 * @brief Refresh file list in current directory
 * 
 * @param browser Pointer to file browser structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t file_browser_refresh(file_browser_t *browser);
esp_err_t file_browser_apply_filter(file_browser_t *browser);
void file_browser_set_search_mode(file_browser_t *browser, bool active);
void file_browser_clear_search(file_browser_t *browser);
void file_browser_append_search_char(file_browser_t *browser, char ch);
void file_browser_backspace_search(file_browser_t *browser);
bool file_browser_jump_to_letter(file_browser_t *browser, char ch);
void file_browser_free(file_browser_t *browser);
void file_browser_page_move(file_browser_t *browser, int direction, int page_size);
void file_browser_remember_selection(file_browser_t *browser);
void file_browser_restore_selection(file_browser_t *browser);
void file_browser_cycle_sort(file_browser_t *browser);
void file_browser_build_search_cache_async(file_browser_t *browser, const char *root_path);
void file_browser_fill_cached_meta(file_browser_t *browser, file_entry_t *entry);

/**
 * @brief Move selection up
 * 
 * @param browser Pointer to file browser structure
 */
void file_browser_up(file_browser_t *browser);

/**
 * @brief Move selection down
 * 
 * @param browser Pointer to file browser structure
 */
void file_browser_down(file_browser_t *browser);

/**
 * @brief Enter selected directory or return selected file path
 * 
 * @param browser Pointer to file browser structure
 * @param selected_path Output buffer for selected path
 * @param path_len Size of output buffer
 * @return esp_err_t ESP_OK if file selected, ESP_ERR_INVALID_STATE if directory entered
 */
esp_err_t file_browser_select(file_browser_t *browser, char *selected_path, size_t path_len);

/**
 * @brief Go back to parent directory
 * 
 * @param browser Pointer to file browser structure
 * @return esp_err_t ESP_OK on success
 */
esp_err_t file_browser_back(file_browser_t *browser);

/**
 * @brief Get currently selected filename
 * 
 * @param browser Pointer to file browser structure
 * @return const char* Filename or NULL if none selected
 */
const char *file_browser_get_selected(file_browser_t *browser);
