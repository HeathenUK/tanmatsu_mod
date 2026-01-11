#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xmp.h"  // For xmp_frame_info

/**
 * @brief Initialize MOD player
 * 
 * @param sample_rate Sample rate for playback (typically 44100)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mod_player_init(uint32_t sample_rate);

/**
 * @brief Load and start playing a MOD file from memory
 * 
 * @param mod_data Pointer to MOD file data
 * @param mod_size Size of MOD file in bytes
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mod_player_load(const uint8_t* mod_data, size_t mod_size);

/**
 * @brief Start MOD playback
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mod_player_start(void);

/**
 * @brief Stop MOD playback
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mod_player_stop(void);

/**
 * @brief Check if MOD is currently playing
 * 
 * @return true if playing, false otherwise
 */
bool mod_player_is_playing(void);

/**
 * @brief Get MOD player task handle (for priority/affinity control)
 * 
 * @return TaskHandle_t Task handle, or NULL if not initialized
 */
TaskHandle_t mod_player_get_task_handle(void);

/**
 * @brief Get current playback frame information (for tracker UI)
 * 
 * @param frame_info Pointer to store frame info (can be NULL if not needed)
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not playing
 */
esp_err_t mod_player_get_frame_info(struct xmp_frame_info *frame_info);

/**
 * @brief Get module information (for tracker UI)
 * 
 * @param mod_info Pointer to store module info
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not loaded
 */
esp_err_t mod_player_get_module_info(struct xmp_module_info *mod_info);
