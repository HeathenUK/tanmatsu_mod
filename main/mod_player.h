#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// Forward declarations for compatibility structures
// These match libxmp's structures but are provided by our wrapper
struct xmp_frame_info;
struct xmp_module_info;
struct xmp_module;

/**
 * @brief Initialize MOD player
 * 
 * @param sample_rate Sample rate for playback (typically 22050 or 44100)
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

/**
 * @brief Toggle mute state of a channel
 * 
 * @param channel Channel number (0-based)
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not playing, ESP_ERR_INVALID_ARG if invalid channel
 */
esp_err_t mod_player_toggle_channel_mute(int channel);

/**
 * @brief Check if a channel is muted
 * 
 * @param channel Channel number (0-based)
 * @param is_muted Pointer to store mute state
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not playing, ESP_ERR_INVALID_ARG if invalid channel
 */
esp_err_t mod_player_get_channel_mute(int channel, bool *is_muted);

/**
 * @brief Get pattern row data for a specific channel (for previewing future rows)
 * 
 * @param pattern Pattern index
 * @param row Row index within pattern
 * @param channel Channel number (0-based)
 * @param note Output: Note value (0 = no note, 1-96 = note)
 * @param ins Output: Instrument number (0 = no instrument, 1+ = instrument)
 * @param fxt Output: Effect type (0-255)
 * @param fxp Output: Effect parameter (0-255)
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not loaded, ESP_ERR_INVALID_ARG if invalid pattern/row/channel, ESP_ERR_NOT_SUPPORTED if pattern access not available (e.g., libxmp limitation)
 */
esp_err_t mod_player_get_pattern_row_channel(int pattern, int row, int channel, 
                                              uint8_t *note, uint8_t *ins, 
                                              uint8_t *fxt, uint8_t *fxp);

/**
 * @brief Get number of rows in a pattern
 * 
 * @param pattern Pattern index
 * @param num_rows Output: Number of rows in the pattern
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not loaded, ESP_ERR_INVALID_ARG if invalid pattern
 */
esp_err_t mod_player_get_pattern_num_rows(int pattern, int *num_rows);

/**
 * @brief Get pattern number for a given order position
 * 
 * @param order Order position (0-based)
 * @param pattern Output: Pattern number at this order position
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not loaded, ESP_ERR_INVALID_ARG if invalid order
 */
esp_err_t mod_player_get_order_pattern(int order, int *pattern);
