#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define SD_CARD_MOUNT_POINT "/sdcard"

/**
 * @brief Initialize and mount SD card
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sdcard_init(void);

/**
 * @brief Check if SD card is mounted
 * 
 * @return true if mounted, false otherwise
 */
bool sdcard_is_mounted(void);
