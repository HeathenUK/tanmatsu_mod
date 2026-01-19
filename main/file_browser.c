#include "file_browser.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>

static const char *TAG = "file_browser";

static int compare_entries(const void *a, const void *b) {
    const file_entry_t *entry_a = (const file_entry_t *)a;
    const file_entry_t *entry_b = (const file_entry_t *)b;
    
    // Directories first
    if (entry_a->is_dir && !entry_b->is_dir) return -1;
    if (!entry_a->is_dir && entry_b->is_dir) return 1;
    
    // Then alphabetical
    return strcasecmp(entry_a->filename, entry_b->filename);
}

esp_err_t file_browser_init(file_browser_t *browser, const char *base_path) {
    if (browser == NULL || base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(browser, 0, sizeof(file_browser_t));
    strncpy(browser->current_path, base_path, sizeof(browser->current_path) - 1);
    browser->selected_index = 0;
    
    return file_browser_refresh(browser);
}

esp_err_t file_browser_refresh(file_browser_t *browser) {
    if (browser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    browser->count = 0;
    browser->selected_index = 0;
    
    // Check if we're at root (need to add ".." entry)
    bool is_root = (strcmp(browser->current_path, "/sdcard") == 0);
    
    // Add ".." entry if not at root
    if (!is_root) {
        file_entry_t *file = &browser->files[browser->count];
        strncpy(file->filename, "..", sizeof(file->filename) - 1);
        file->filename[sizeof(file->filename) - 1] = '\0';
        file->is_dir = true;
        browser->count++;
    }
    
    DIR *dir = opendir(browser->current_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", browser->current_path);
        return ESP_ERR_NOT_FOUND;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && browser->count < MAX_FILES) {
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
                file_entry_t *file = &browser->files[browser->count];
                strncpy(file->filename, entry->d_name, sizeof(file->filename) - 1);
                file->filename[sizeof(file->filename) - 1] = '\0';
                file->is_dir = true;
                browser->count++;
            } else {
                // For files, include MOD files (.mod, .xm, .s3m, .it) and VGM files (.vgm, .vgz)
                const char *ext = strrchr(entry->d_name, '.');
                if (ext != NULL && (
                    strcasecmp(ext, ".mod") == 0 ||
                    strcasecmp(ext, ".xm") == 0 ||
                    strcasecmp(ext, ".s3m") == 0 ||
                    strcasecmp(ext, ".it") == 0 ||
                    strcasecmp(ext, ".vgm") == 0 ||
                    strcasecmp(ext, ".vgz") == 0)) {
                    file_entry_t *file = &browser->files[browser->count];
                    strncpy(file->filename, entry->d_name, sizeof(file->filename) - 1);
                    file->filename[sizeof(file->filename) - 1] = '\0';
                    file->is_dir = false;
                    browser->count++;
                }
            }
        }
    }
    closedir(dir);
    
    // Sort: ".." first (already at index 0), then directories, then files (alphabetical)
    if (browser->count > 0) {
        int start_idx = is_root ? 0 : 1;  // Skip ".." entry if present
        if (start_idx < browser->count) {
            qsort(browser->files + start_idx, browser->count - start_idx, sizeof(file_entry_t), compare_entries);
        }
    }
    
    ESP_LOGI(TAG, "Found %d entries in %s", browser->count, browser->current_path);
    return ESP_OK;
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
    
    // Build full path
    snprintf(selected_path, path_len, "%s/%s", browser->current_path, entry->filename);
    
    if (entry->is_dir) {
        // Enter directory
        strncpy(browser->current_path, selected_path, sizeof(browser->current_path) - 1);
        browser->current_path[sizeof(browser->current_path) - 1] = '\0';
        browser->selected_index = 0;
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
    return file_browser_refresh(browser);
}

const char *file_browser_get_selected(file_browser_t *browser) {
    if (browser == NULL || browser->count == 0 || browser->selected_index >= browser->count) {
        return NULL;
    }
    return browser->files[browser->selected_index].filename;
}
