#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define MAX_FILENAME_LEN 64   // Match FATFS_MAX_LFN to save space
#define MAX_FILES 32          // Reduced from 64 to save space

typedef struct {
    char filename[MAX_FILENAME_LEN];
    bool is_dir;
} file_entry_t;

typedef struct {
    file_entry_t files[MAX_FILES];
    int count;
    int selected_index;
    char current_path[MAX_FILENAME_LEN];
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
