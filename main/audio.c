#include "audio.h"
#include "bsp/audio.h"
#include "bsp/i2c.h"
#include "bsp/tanmatsu.h"
#include "driver/i2s_std.h"
#include "es8156.h"
#include "esp_log.h"
#include "math.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mod_backend_config.h"

static const char *TAG = "audio";

// External BSP audio function (not in public header)
extern void bsp_audio_initialize(uint32_t rate);

// ES8156 codec handle for diagnostics and optimization
// Commented out - using BSP functions instead
// static es8156_handle_t es8156_handle = NULL;

// I2S configuration - use the same sample rate as MOD backend
#define I2S_SAMPLE_RATE MOD_CONFIG_SAMPLE_RATE
#define I2S_BITS_PER_SAMPLE 16
#define I2S_CHANNELS 2

static i2s_chan_handle_t i2s_handle = NULL;
static bool audio_initialized = false;
static bool amplifier_enabled = false;
static uint8_t current_volume = 100;  // 0-100%

esp_err_t audio_init(void) {
    if (audio_initialized) {
        ESP_LOGW(TAG, "Audio already initialized");
        return ESP_OK;
    }

    // Initialize audio subsystem using BSP
    // The BSP handles all GPIO pin configuration automatically
    // Note: bsp_audio_initialize() currently hardcodes 44100 Hz, so we need to call
    // bsp_audio_set_rate() after initialization to set the desired rate
    bsp_audio_initialize(I2S_SAMPLE_RATE);
    
    // Get the I2S handle from BSP (must be done before disabling channel)
    bsp_audio_get_i2s_handle(&i2s_handle);
    
    if (i2s_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get I2S handle from BSP");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Set the actual sample rate (bsp_audio_initialize hardcodes 44100, ignore rate param)
    // Note: I2S channel must be disabled before reconfiguring the clock
    i2s_channel_disable(i2s_handle);
    esp_err_t set_rate_ret = bsp_audio_set_rate(I2S_SAMPLE_RATE);
    if (set_rate_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set audio sample rate to %d Hz: %s", I2S_SAMPLE_RATE, esp_err_to_name(set_rate_ret));
        return set_rate_ret;
    }
    // Re-enable the channel after reconfiguring
    esp_err_t enable_ret = i2s_channel_enable(i2s_handle);
    if (enable_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-enable I2S channel after rate change: %s", esp_err_to_name(enable_ret));
        return enable_ret;
    }

    // Enable amplifier (required for sound output)
    bsp_audio_set_amplifier(true);
    amplifier_enabled = true;

    // Get initial volume from BSP - BSP should handle persistence
    float volume_percent = 70.0f;  // Default fallback if BSP doesn't support get_volume
    esp_err_t get_vol_ret = bsp_audio_get_volume(&volume_percent);
    if (get_vol_ret == ESP_OK) {
        ESP_LOGI(TAG, "Got initial volume from BSP: %.1f%%", volume_percent);
    } else if (get_vol_ret == ESP_ERR_NOT_SUPPORTED) {
        // BSP doesn't support get_volume, use default 70%
        ESP_LOGI(TAG, "BSP get_volume not supported, using default: 70%%");
        volume_percent = 70.0f;
    } else {
        // Other error, still use default but log it
        ESP_LOGW(TAG, "bsp_audio_get_volume returned: %s, using default: 70%%", esp_err_to_name(get_vol_ret));
        volume_percent = 70.0f;
    }
    current_volume = (uint8_t)volume_percent;

    // Get ES8156 handle for diagnostics and optimization
    // Commented out - using BSP functions instead
    // Re-initialize access using the same I2C bus as BSP
    // i2c_master_bus_handle_t i2c_bus_handle = NULL;
    // SemaphoreHandle_t i2c_bus_semaphore = NULL;
    // if (bsp_i2c_primary_bus_get_handle(&i2c_bus_handle) == ESP_OK &&
    //     bsp_i2c_primary_bus_get_semaphore(&i2c_bus_semaphore) == ESP_OK) {
    //     es8156_config_t es8156_config = {
    //         .i2c_bus = i2c_bus_handle,
    //         .i2c_address = 0x08,  // ES8156 I2C address (BSP_ES8156_I2C_ADDRESS)
    //         .concurrency_semaphore = i2c_bus_semaphore,
    //     };
    //     esp_err_t es8156_ret = es8156_initialize(&es8156_config, &es8156_handle);
    //     if (es8156_ret == ESP_OK) {
    //         ESP_LOGI(TAG, "ES8156 handle acquired for diagnostics");
    //         // Optimize ES8156 settings for better audio quality
    //         audio_optimize_es8156();
    //     } else {
    //         ESP_LOGW(TAG, "Failed to acquire ES8156 handle (diagnostics unavailable): %s", esp_err_to_name(es8156_ret));
    //     }
    // } else {
    //     ESP_LOGW(TAG, "Failed to get I2C bus handle/semaphore (ES8156 diagnostics unavailable)");
    // }

    audio_initialized = true;
    ESP_LOGI(TAG, "Audio system initialized (sample rate: %d Hz)", I2S_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_play_tone(float frequency, uint32_t duration_ms, float volume) {
    if (!audio_initialized || i2s_handle == NULL) {
        ESP_LOGE(TAG, "Audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (frequency < 20.0f || frequency > 20000.0f) {
        ESP_LOGE(TAG, "Invalid frequency: %.1f Hz", frequency);
        return ESP_ERR_INVALID_ARG;
    }

    if (volume < 0.0f || volume > 1.0f) {
        ESP_LOGE(TAG, "Invalid volume: %.2f", volume);
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate number of samples
    uint32_t num_samples = (I2S_SAMPLE_RATE * duration_ms) / 1000;
    size_t buffer_size = num_samples * I2S_CHANNELS * (I2S_BITS_PER_SAMPLE / 8);
    int16_t *buffer = malloc(buffer_size);

    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return ESP_ERR_NO_MEM;
    }

    // Generate sine wave
    float phase_increment = 2.0f * M_PI * frequency / I2S_SAMPLE_RATE;
    float phase = 0.0f;
    int16_t max_amplitude = (int16_t)(32767.0f * volume);

    for (uint32_t i = 0; i < num_samples; i++) {
        int16_t sample = (int16_t)(sinf(phase) * max_amplitude);
        buffer[i * I2S_CHANNELS + 0] = sample;     // Left channel
        buffer[i * I2S_CHANNELS + 1] = sample;     // Right channel
        phase += phase_increment;
        if (phase >= 2.0f * M_PI) {
            phase -= 2.0f * M_PI;
        }
    }

    // Write to I2S
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(i2s_handle, buffer, buffer_size, &bytes_written, portMAX_DELAY);

    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write audio data: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t audio_beep(uint32_t duration_ms) {
    return audio_play_tone(440.0f, duration_ms, 1.0f);  // A4 note at 100% volume (was 50%)
}

esp_err_t audio_stop(void) {
    if (!audio_initialized || i2s_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Write silence to drain the DMA buffer and prevent clicks
    // Calculate buffer size (typically DMA buffer is around 8KB, write a bit more to be safe)
    size_t silence_size = 4096;  // 2KB of stereo 16-bit samples
    int16_t *silence_buffer = malloc(silence_size);
    if (silence_buffer != NULL) {
        memset(silence_buffer, 0, silence_size);
        
        // Write silence non-blocking a few times to drain
        for (int i = 0; i < 3; i++) {
            size_t bytes_written = 0;
            i2s_channel_write(i2s_handle, silence_buffer, silence_size, &bytes_written, 0);
        }
        
        free(silence_buffer);
    }
    
    // Small delay to let silence play out
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Disable the channel to stop output
    i2s_channel_disable(i2s_handle);

    return ESP_OK;
}

esp_err_t audio_set_volume(float volume) {
    if (volume < 0.0f || volume > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Convert 0.0-1.0 to 0-100% (BSP expects float)
    float volume_percent = volume * 100.0f;
    
    esp_err_t ret = bsp_audio_set_volume(volume_percent);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(ret));
        return ret;
    }

    current_volume = (uint8_t)volume_percent;
    ESP_LOGI(TAG, "Volume set to %.0f%%", volume_percent);
    return ESP_OK;
}

esp_err_t audio_get_volume(float *volume) {
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Try to query BSP for volume, but fall back to cached value if not supported
    float volume_percent = 0.0f;
    esp_err_t ret = bsp_audio_get_volume(&volume_percent);
    if (ret == ESP_OK) {
        // BSP returned volume, convert from 0-100% to 0.0-1.0
        *volume = volume_percent / 100.0f;
        current_volume = (uint8_t)volume_percent;  // Update cached value for display
        return ESP_OK;
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        // BSP doesn't support get_volume, use cached value (no log spam)
        *volume = current_volume / 100.0f;
        return ESP_OK;
    }
    
    // Other error - log once but still return cached value as fallback
    static bool logged_get_volume_error = false;
    if (!logged_get_volume_error) {
        ESP_LOGW(TAG, "bsp_audio_get_volume failed: %s, using cached volume", esp_err_to_name(ret));
        logged_get_volume_error = true;
    }
    *volume = current_volume / 100.0f;
    return ESP_OK;
}

esp_err_t audio_get_i2s_handle(void **handle) {
    if (!audio_initialized || i2s_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *handle = (void *)i2s_handle;
    return ESP_OK;
}

void audio_diagnose_es8156(void) {
    ESP_LOGW(TAG, "audio_diagnose_es8156: ES8156 diagnostics disabled (using BSP)");
}

esp_err_t audio_optimize_es8156(void) {
    ESP_LOGW(TAG, "audio_optimize_es8156: ES8156 optimization disabled (using BSP)");
    return ESP_ERR_NOT_SUPPORTED;
}
