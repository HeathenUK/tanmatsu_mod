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

static const char *TAG = "audio";

// External BSP audio function (not in public header)
extern void bsp_audio_initialize(uint32_t rate);

// ES8156 codec handle for diagnostics and optimization
static es8156_handle_t es8156_handle = NULL;

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

    // Get ES8156 handle for diagnostics and optimization
    // Re-initialize access using the same I2C bus as BSP
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    SemaphoreHandle_t i2c_bus_semaphore = NULL;
    if (bsp_i2c_primary_bus_get_handle(&i2c_bus_handle) == ESP_OK &&
        bsp_i2c_primary_bus_get_semaphore(&i2c_bus_semaphore) == ESP_OK) {
        es8156_config_t es8156_config = {
            .i2c_bus = i2c_bus_handle,
            .i2c_address = 0x08,  // ES8156 I2C address (BSP_ES8156_I2C_ADDRESS)
            .concurrency_semaphore = i2c_bus_semaphore,
        };
        esp_err_t es8156_ret = es8156_initialize(&es8156_config, &es8156_handle);
        if (es8156_ret == ESP_OK) {
            ESP_LOGI(TAG, "ES8156 handle acquired for diagnostics");
            // Optimize ES8156 settings for better audio quality
            audio_optimize_es8156();
        } else {
            ESP_LOGW(TAG, "Failed to acquire ES8156 handle (diagnostics unavailable): %s", esp_err_to_name(es8156_ret));
        }
    } else {
        ESP_LOGW(TAG, "Failed to get I2C bus handle/semaphore (ES8156 diagnostics unavailable)");
    }

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

void audio_diagnose_es8156(void) {
    if (es8156_handle == NULL) {
        ESP_LOGW(TAG, "ES8156 handle not available for diagnostics");
        return;
    }

    ESP_LOGI(TAG, "=== ES8156 Diagnostic Report ===");
    
    // Read automute control
    uint8_t automute_size = 0, automute_ng = 0;
    if (es8156_read_automute_control(es8156_handle, &automute_size, &automute_ng) == ESP_OK) {
        ESP_LOGI(TAG, "Automute: size=%u, ng=%u", automute_size, automute_ng);
    }
    
    // Read ALC configuration
    bool dac_alc_en = false;
    uint8_t alc_mute_gain = 0;
    if (es8156_read_alc_config_1(es8156_handle, &dac_alc_en, &alc_mute_gain) == ESP_OK) {
        ESP_LOGI(TAG, "ALC: enabled=%d, mute_gain=%u", dac_alc_en, alc_mute_gain);
    }
    
    uint8_t alc_win_size = 0, alc_ramp_rate = 0;
    if (es8156_read_alc_config_2(es8156_handle, &alc_win_size, &alc_ramp_rate) == ESP_OK) {
        ESP_LOGI(TAG, "ALC: win_size=%u, ramp_rate=%u", alc_win_size, alc_ramp_rate);
    }
    
    uint8_t alc_minlevel = 0, alc_maxlevel = 0;
    if (es8156_read_alc_level(es8156_handle, &alc_minlevel, &alc_maxlevel) == ESP_OK) {
        ESP_LOGI(TAG, "ALC: minlevel=%u, maxlevel=%u", alc_minlevel, alc_maxlevel);
    }
    
    // Read time control
    uint8_t v_t1 = 0, v_t2 = 0;
    if (es8156_read_time_control_1(es8156_handle, &v_t1) == ESP_OK) {
        ESP_LOGI(TAG, "Time Control 1: v_t1=%u", v_t1);
    }
    if (es8156_read_time_control_2(es8156_handle, &v_t2) == ESP_OK) {
        ESP_LOGI(TAG, "Time Control 2: v_t2=%u", v_t2);
    }
    
    // Read volume control
    uint8_t dac_volume_db = 0;
    if (es8156_read_volume_control(es8156_handle, &dac_volume_db) == ESP_OK) {
        ESP_LOGI(TAG, "Volume: %u dB", dac_volume_db);
    }
    
    // Read mute control
    bool am_ena = false, lch_dsm_smute = false, rch_dsm_smute = false;
    bool am_dsmmute_ena = false, am_aclkoff_ena = false;
    bool am_attenu6_ena = false, intout_clipen = false;
    if (es8156_read_mute_control(es8156_handle, &am_ena, &lch_dsm_smute, &rch_dsm_smute,
                                 &am_dsmmute_ena, &am_aclkoff_ena, &am_attenu6_ena, &intout_clipen) == ESP_OK) {
        ESP_LOGI(TAG, "Mute Control: am_ena=%d, lch_smute=%d, rch_smute=%d, clipen=%d",
                am_ena, lch_dsm_smute, rch_dsm_smute, intout_clipen);
    }
    
    // Read misc control 3 (DSM dithering)
    bool dac_ram_clr = false, dsm_ditheron = false;
    bool rch_inv = false, lch_inv = false;
    uint8_t chn_cross = 0;
    bool p2s_dpath_sel = false, p2s_data_bitnum = false;
    if (es8156_read_misc_control_3(es8156_handle, &dac_ram_clr, &dsm_ditheron, &rch_inv, &lch_inv,
                                   &chn_cross, &p2s_dpath_sel, &p2s_data_bitnum) == ESP_OK) {
        ESP_LOGI(TAG, "Misc Control 3: dsm_ditheron=%d", dsm_ditheron);
    }
    
    ESP_LOGI(TAG, "==================================");
}

esp_err_t audio_optimize_es8156(void) {
    if (es8156_handle == NULL) {
        ESP_LOGW(TAG, "ES8156 handle not available for optimization");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Optimizing ES8156 settings for better audio quality...");
    
    esp_err_t ret = ESP_OK;
    
    // 1. Disable automute (can cause crackling on silence/noise detection)
    ret = es8156_write_automute_control(es8156_handle, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable automute: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Automute disabled");
    }
    
    // 2. Disable ALC (Automatic Level Control) - can cause artifacts
    ret = es8156_write_alc_config_1(es8156_handle, false, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable ALC: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "ALC disabled");
    }
    
    // 3. Increase time control values for more stable power sequencing
    ret = es8156_write_time_control_1(es8156_handle, 4);  // Increased from 1 to 4
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set time control 1: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Time Control 1 set to 4");
    }
    
    ret = es8156_write_time_control_2(es8156_handle, 4);  // Increased from 1 to 4
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set time control 2: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Time Control 2 set to 4");
    }
    
    // 4. Reduce maximum volume to prevent clipping (85 dB instead of 100 dB)
    // Note: BSP volume control already handles volume scaling
    ret = es8156_write_volume_control(es8156_handle, 85);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set volume: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Base volume set to 85 dB (was 100 dB)");
    }
    
    // 5. Enable DSM dithering to reduce artifacts
    bool dac_ram_clr, dsm_ditheron, rch_inv, lch_inv;
    uint8_t chn_cross;
    bool p2s_dpath_sel, p2s_data_bitnum;
    ret = es8156_read_misc_control_3(es8156_handle, &dac_ram_clr, &dsm_ditheron, &rch_inv, &lch_inv,
                                     &chn_cross, &p2s_dpath_sel, &p2s_data_bitnum);
    if (ret == ESP_OK) {
        ret = es8156_write_misc_control_3(es8156_handle, dac_ram_clr, true, rch_inv, lch_inv,
                                          chn_cross, p2s_dpath_sel, p2s_data_bitnum);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to enable DSM dithering: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "DSM dithering enabled");
        }
    }
    
    // 6. Ensure mute control is properly configured
    bool am_ena, lch_dsm_smute, rch_dsm_smute;
    bool am_dsmmute_ena, am_aclkoff_ena, am_attenu6_ena, intout_clipen;
    ret = es8156_read_mute_control(es8156_handle, &am_ena, &lch_dsm_smute, &rch_dsm_smute,
                                   &am_dsmmute_ena, &am_aclkoff_ena, &am_attenu6_ena, &intout_clipen);
    if (ret == ESP_OK) {
        // Ensure channels are not muted
        ret = es8156_write_mute_control(es8156_handle, am_ena, false, false,
                                       am_dsmmute_ena, am_aclkoff_ena, am_attenu6_ena, intout_clipen);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to configure mute control: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Mute control configured (channels unmuted)");
        }
    }
    
    ESP_LOGI(TAG, "ES8156 optimization complete");
    return ESP_OK;
}
