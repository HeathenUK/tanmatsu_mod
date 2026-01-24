#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Initialize audio system (I2S and ES8156 codec)
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_init(void);

/**
 * @brief Play a tone at specified frequency and duration
 * 
 * @param frequency Frequency in Hz (e.g., 440 for A4)
 * @param duration_ms Duration in milliseconds
 * @param volume Volume level (0.0 to 1.0)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_play_tone(float frequency, uint32_t duration_ms, float volume);

/**
 * @brief Play a beep (short tone at 440Hz)
 * 
 * @param duration_ms Duration in milliseconds
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_beep(uint32_t duration_ms);

/**
 * @brief Stop audio playback
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_stop(void);
esp_err_t audio_set_sample_rate(uint32_t rate);

typedef enum {
    AUDIO_OUTPUT_OWNER_NONE = 0,
    AUDIO_OUTPUT_OWNER_MOD,
    AUDIO_OUTPUT_OWNER_VGM,
} audio_output_owner_t;

bool audio_output_acquire(audio_output_owner_t owner);
void audio_output_release(audio_output_owner_t owner);
bool audio_output_is_owner(audio_output_owner_t owner);
audio_output_owner_t audio_output_get_owner(void);

/**
 * @brief Set audio volume
 * 
 * @param volume Volume level (0.0 to 1.0)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_set_volume(float volume);

/**
 * @brief Get current audio volume
 * 
 * @param volume Pointer to store volume level (0.0 to 1.0)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_get_volume(float *volume);

/**
 * @brief Get I2S handle for direct audio operations
 * 
 * @param handle Pointer to store I2S handle
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_get_i2s_handle(void **handle);

/**
 * @brief Diagnose ES8156 codec configuration (read and log current settings)
 */
void audio_diagnose_es8156(void);

/**
 * @brief Optimize ES8156 codec settings for better audio quality
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t audio_optimize_es8156(void);
