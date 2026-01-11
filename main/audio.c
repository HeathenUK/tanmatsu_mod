#include "audio.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "math.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// External BSP audio function (not in public header)
extern void bsp_audio_initialize(uint32_t rate);

// I2S configuration
#define I2S_SAMPLE_RATE 44100
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
    bsp_audio_initialize(I2S_SAMPLE_RATE);
    
    // Get the I2S handle from BSP
    bsp_audio_get_i2s_handle(&i2s_handle);
    
    if (i2s_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get I2S handle from BSP");
        return ESP_ERR_INVALID_STATE;
    }

    // Enable amplifier (required for sound output)
    bsp_audio_set_amplifier(true);
    amplifier_enabled = true;

    // Set initial volume to 70%
    bsp_audio_set_volume(70);
    current_volume = 70;

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

    // Convert 0.0-1.0 to 0-100%
    uint8_t volume_percent = (uint8_t)(volume * 100.0f);
    
    esp_err_t ret = bsp_audio_set_volume(volume_percent);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(ret));
        return ret;
    }

    current_volume = volume_percent;
    ESP_LOGI(TAG, "Volume set to %d%%", volume_percent);
    return ESP_OK;
}

esp_err_t audio_get_volume(float *volume) {
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Convert 0-100% to 0.0-1.0
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
