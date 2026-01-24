#pragma once

/**
 * @file vgm_player.h
 * @brief VGM/VGZ player module
 *
 * High-level VGM playback interface that mirrors mod_player.h patterns.
 * Uses vgm_backend for libvgm integration and audio.c for I2S output.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vgm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize VGM player
 *
 * Creates playback task and initializes backend.
 * Must be called after audio_init().
 *
 * @param sample_rate Sample rate for playback (typically 44100)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t vgm_player_init(uint32_t sample_rate);

/**
 * @brief Load VGM/VGZ file from memory
 *
 * Stops any current playback before loading.
 *
 * @param vgm_data Pointer to VGM/VGZ file data
 * @param vgm_size Size of file in bytes
 * @return esp_err_t ESP_OK on success
 */
esp_err_t vgm_player_load(const uint8_t *vgm_data, size_t vgm_size);

/**
 * @brief Start VGM playback
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not loaded
 */
esp_err_t vgm_player_start(void);

/**
 * @brief Stop VGM playback
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t vgm_player_stop(void);

/**
 * @brief Pause VGM playback
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not playing
 */
esp_err_t vgm_player_pause(void);

/**
 * @brief Resume VGM playback
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not paused
 */
esp_err_t vgm_player_resume(void);

/**
 * @brief Skip ahead by N seconds during playback (rendering without output)
 *
 * @param seconds Seconds to skip forward
 * @return ESP_OK on success
 */
esp_err_t vgm_player_skip_seconds(uint32_t seconds);

/**
 * @brief Check if VGM is currently playing
 *
 * @return true if playing, false otherwise
 */
bool vgm_player_is_playing(void);

/**
 * @brief Check if VGM playback is paused
 *
 * @return true if paused, false otherwise
 */
bool vgm_player_is_paused(void);

/**
 * @brief Check if a VGM file is loaded
 *
 * @return true if loaded, false otherwise
 */
bool vgm_player_is_loaded(void);

/**
 * @brief Get last VGM player error message (if any)
 *
 * @return Pointer to error string, or NULL if none
 */
const char *vgm_player_get_last_error(void);

/**
 * @brief Get VGM player task handle
 *
 * @return TaskHandle_t Task handle, or NULL if not initialized
 */
TaskHandle_t vgm_player_get_task_handle(void);

/**
 * @brief Get GD3 tags (metadata)
 *
 * @param tags Pointer to tags structure to fill
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if not loaded
 */
esp_err_t vgm_player_get_tags(vgm_tags_t *tags);

/**
 * @brief Get playback information
 *
 * @param info Pointer to info structure to fill
 * @return esp_err_t ESP_OK on success
 */
esp_err_t vgm_player_get_info(vgm_playback_info_t *info);

/**
 * @brief Get chip information
 *
 * @param index Chip index (0 to num_chips-1)
 * @param chip_info Pointer to chip info structure to fill
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t vgm_player_get_chip_info(uint8_t index, vgm_chip_info_t *chip_info);

/**
 * @brief Set number of loops
 *
 * @param loops Number of loops (0 = infinite, 1 = play once, 2 = loop once, etc.)
 */
void vgm_player_set_loop_count(uint32_t loops);

/**
 * @brief Set fade out time
 *
 * @param fade_ms Fade out duration in milliseconds
 */
void vgm_player_set_fade_time(uint32_t fade_ms);

/**
 * @brief Get waveform data for visualization
 *
 * Returns the most recent audio samples for oscilloscope display.
 * Data is decimated from the render buffer to fit the requested size.
 *
 * @param left Output buffer for left channel samples
 * @param right Output buffer for right channel samples
 * @param max_samples Maximum samples to return (size of output buffers)
 * @param out_samples Actual number of samples written
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no data available
 */
esp_err_t vgm_player_get_waveform(int16_t *left, int16_t *right, size_t max_samples, size_t *out_samples);

#ifdef __cplusplus
}
#endif
