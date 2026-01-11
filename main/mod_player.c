#include "mod_player.h"
#include "audio.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// libxmp header (lite subset via LIBXMP_CORE_PLAYER define)
#include "xmp.h"

static const char *TAG = "mod_player";

// MOD player state
static xmp_context mod_ctx = NULL;
static i2s_chan_handle_t i2s_handle = NULL;
static uint32_t sample_rate = 44100;
static bool mod_loaded = false;
static bool mod_playing = false;
static TaskHandle_t mod_task_handle = NULL;

// Task stack in PSRAM (much larger than SRAM allows)
#define MOD_TASK_STACK_SIZE (16 * 1024)  // 16KB stack in PSRAM
static StackType_t *mod_task_stack = NULL;
static StaticTask_t *mod_task_tcb = NULL;

// Audio buffer for MOD playback (4096 samples for maximum buffering against CPU contention)
// 512 samples = ~11.6ms, 1024 = ~23.2ms, 2048 = ~46.4ms, 4096 = ~92.9ms at 44.1kHz
// Large buffer provides significant headroom for UI operations (scrolling, rendering, PPA rotation)
#define MOD_BUFFER_SAMPLES 4096
#define MOD_BUFFER_SIZE (MOD_BUFFER_SAMPLES * 2 * sizeof(int16_t))  // Stereo 16-bit

/**
 * @brief MOD playback task - renders MOD audio and writes to I2S
 */
static void mod_playback_task(void *arg) {
    // Use static buffers to reduce stack usage
    // Initialize to zero to prevent garbage audio output
    static int16_t mono_buffer[MOD_BUFFER_SAMPLES] = {0};
    static int16_t stereo_buffer[MOD_BUFFER_SAMPLES * 2] = {0};
    size_t bytes_written;
    uint32_t buffer_count = 0;
    // uint32_t last_log_time = 0;  // Commented out - logging disabled

    // ESP_LOGI(TAG, "MOD playback task started");  // Commented out - logging disabled

    while (1) {
        // Only play if MOD is loaded, playing flag is set, and we have valid handles
        if (mod_playing && mod_loaded && mod_ctx && i2s_handle) {
            // Render MOD audio (mono, 16-bit)
            // xmp_play_buffer expects buffer size in bytes (samples * sizeof(int16_t))
            int rc = xmp_play_buffer(mod_ctx, mono_buffer, sizeof(mono_buffer), 1);
            
            if (rc == 0) {
                // First write after starting - logging commented out
                // static bool first_write = true;
                // if (first_write) {
                //     ESP_LOGI(TAG, "First MOD audio buffer written to I2S");
                //     // Check if buffer actually contains audio data (not all zeros)
                //     int16_t max_sample = 0;
                //     int16_t min_sample = 0;
                //     for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                //         if (mono_buffer[i] > max_sample) max_sample = mono_buffer[i];
                //         if (mono_buffer[i] < min_sample) min_sample = mono_buffer[i];
                //     }
                //     ESP_LOGI(TAG, "First buffer sample range: min=%d, max=%d", min_sample, max_sample);
                //     first_write = false;
                // }
                
                // Convert mono to stereo (duplicate L/R)
                // Match the beep's format: buffer[i * I2S_CHANNELS + 0] for L, +1 for R
                for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                    stereo_buffer[i * 2 + 0] = mono_buffer[i];  // Left channel
                    stereo_buffer[i * 2 + 1] = mono_buffer[i];  // Right channel
                }
                
                // Pre-fill I2S buffer with a few buffers to ensure continuous playback
                // (I2S DMA might need multiple buffers to start output)
                static bool first_fill = true;
                if (first_fill) {
                    first_fill = false;
                    // Write 4 buffers quickly to fill DMA buffer
                    for (int j = 0; j < 4; j++) {
                        size_t temp_written = 0;
                        i2s_channel_write(i2s_handle, stereo_buffer, sizeof(stereo_buffer), 
                                         &temp_written, 0);  // Non-blocking for quick fill
                        if (temp_written < sizeof(stereo_buffer)) {
                            break;  // Buffer full, stop pre-filling
                        }
                        // Get next buffer from libxmp for pre-fill
                        int pre_rc = xmp_play_buffer(mod_ctx, mono_buffer, sizeof(mono_buffer), 1);
                        if (pre_rc != 0) break;
                        for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                            stereo_buffer[i * 2 + 0] = mono_buffer[i];
                            stereo_buffer[i * 2 + 1] = mono_buffer[i];
                        }
                    }
                    // ESP_LOGI(TAG, "I2S buffer pre-filled");
                }

                // Write to I2S (blocking, same as beep - ensures continuous playback)
                // Channel is already enabled by audio_init(), no need to re-enable
                i2s_channel_write(i2s_handle, stereo_buffer, 
                                   sizeof(stereo_buffer), 
                                   &bytes_written, 
                                   portMAX_DELAY);  // Blocking to ensure continuous audio
                
                buffer_count++;
                
                // No yield needed - blocking I2S write already handles timing
                // Large buffer (4096 samples ~93ms) provides sufficient headroom
                // Logging commented out - removed ret check and all logging statements
            } else {
                // Playback ended or error
                // ESP_LOGI(TAG, "MOD playback ended (rc=%d) after %lu buffers", rc, buffer_count);
                mod_playing = false;
                buffer_count = 0;
            }
        } else {
            // Not playing, write silence to prevent audio glitches
            if (i2s_handle) {
                memset(stereo_buffer, 0, sizeof(stereo_buffer));
                i2s_channel_write(i2s_handle, stereo_buffer, sizeof(stereo_buffer), &bytes_written, 0);  // Non-blocking
            }
            buffer_count = 0;
            // last_log_time = 0;  // Commented out - logging disabled
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t mod_player_init(uint32_t sample_rate_in) {
    if (mod_ctx != NULL) {
        ESP_LOGW(TAG, "MOD player already initialized");
        return ESP_OK;
    }

    sample_rate = sample_rate_in;

    // Get I2S handle from audio system
    // Note: audio_init() must be called first
    esp_err_t ret = audio_get_i2s_handle((void **)&i2s_handle);
    if (ret != ESP_OK || i2s_handle == NULL) {
        ESP_LOGE(TAG, "I2S handle not available - call audio_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Create XMP context
    mod_ctx = xmp_create_context();
    if (mod_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to create XMP context");
        return ESP_ERR_NO_MEM;
    }

    // Allocate task stack in PSRAM (much larger than SRAM allows)
    // This allows us to use a 16KB stack without worrying about SRAM constraints
    mod_task_stack = (StackType_t *)heap_caps_malloc(MOD_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    if (mod_task_stack == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task stack in PSRAM");
        xmp_free_context(mod_ctx);
        mod_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate TCB (Task Control Block) - can be in SRAM as it's small
    mod_task_tcb = (StaticTask_t *)pvPortMalloc(sizeof(StaticTask_t));
    if (mod_task_tcb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task TCB");
        heap_caps_free(mod_task_stack);
        mod_task_stack = NULL;
        xmp_free_context(mod_ctx);
        mod_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create MOD playback task with static stack in PSRAM
    // Note: libxmp needs significant stack space for MOD processing
    mod_task_handle = xTaskCreateStaticPinnedToCore(
        mod_playback_task,
        "mod_player",
        MOD_TASK_STACK_SIZE,             // Stack size (16KB in PSRAM)
        NULL,                            // Parameters
        configMAX_PRIORITIES - 2,        // High priority (same as audio mixing)
        mod_task_stack,                  // Stack buffer (in PSRAM)
        mod_task_tcb,                    // TCB buffer
        1                                // Pin to Core 1
    );

    if (mod_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create MOD playback task");
        heap_caps_free(mod_task_stack);
        mod_task_stack = NULL;
        vPortFree(mod_task_tcb);
        mod_task_tcb = NULL;
        xmp_free_context(mod_ctx);
        mod_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "MOD player task created with %d KB stack in PSRAM", MOD_TASK_STACK_SIZE / 1024);

    ESP_LOGI(TAG, "MOD player initialized (sample rate: %lu Hz)", sample_rate);
    return ESP_OK;
}

esp_err_t mod_player_load(const uint8_t* mod_data, size_t mod_size) {
    if (mod_ctx == NULL) {
        ESP_LOGE(TAG, "MOD player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop current playback if any
    if (mod_loaded) {
        mod_player_stop();
        xmp_release_module(mod_ctx);
        mod_loaded = false;
    }

    // Load MOD from memory
    int ret = xmp_load_module_from_memory(mod_ctx, mod_data, mod_size);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load MOD file (error: %d)", ret);
        return ESP_ERR_INVALID_ARG;
    }

    mod_loaded = true;
    ESP_LOGI(TAG, "MOD file loaded (%zu bytes)", mod_size);

    // Get module info
    struct xmp_module_info mod_info;
    xmp_get_module_info(mod_ctx, &mod_info);
    if (mod_info.mod && mod_info.mod->name[0] != '\0') {
        ESP_LOGI(TAG, "Module name: %s", mod_info.mod->name);
    }

    return ESP_OK;
}

esp_err_t mod_player_start(void) {
    if (mod_ctx == NULL || !mod_loaded) {
        ESP_LOGE(TAG, "MOD not loaded");
        return ESP_ERR_INVALID_STATE;
    }

    if (mod_playing) {
        ESP_LOGW(TAG, "MOD already playing");
        return ESP_OK;
    }

    // Configure libxmp settings
    // Limit mixer voices to 32 (default is 128) to reduce CPU load with many channels
    // This must be set before xmp_start_player()
    xmp_set_player(mod_ctx, XMP_PLAYER_VOICES, 32);
    // Enable DSP filtering (lowpass filter)
    xmp_set_player(mod_ctx, XMP_PLAYER_DSP, XMP_DSP_LOWPASS);
    
    // I2S channel should already be enabled by audio_init()
    // Start player (mono output, loop enabled)
    // Note: XMP_FORMAT_MONO = 4, 0 = stereo (see xmp.h)
    int ret = xmp_start_player(mod_ctx, sample_rate, XMP_FORMAT_MONO);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start MOD player (error: %d)", ret);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Configure libxmp settings for audio quality
    // Use spline interpolation (highest quality, reduces aliasing/artifacts)
    xmp_set_player(mod_ctx, XMP_PLAYER_INTERP, XMP_INTERP_SPLINE);
    
    ESP_LOGI(TAG, "MOD player started at %lu Hz, format: MONO, interpolation: SPLINE, voices: 32, DSP: enabled", sample_rate);

    mod_playing = true;
    ESP_LOGI(TAG, "MOD playback started");
    return ESP_OK;
}

esp_err_t mod_player_stop(void) {
    if (mod_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mod_playing) {
        return ESP_OK;
    }

    xmp_end_player(mod_ctx);
    mod_playing = false;
    ESP_LOGI(TAG, "MOD playback stopped");
    return ESP_OK;
}

bool mod_player_is_playing(void) {
    return mod_playing && mod_loaded;
}

TaskHandle_t mod_player_get_task_handle(void) {
    return mod_task_handle;
}

esp_err_t mod_player_get_frame_info(struct xmp_frame_info *frame_info) {
    if (mod_ctx == NULL || !mod_playing || frame_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xmp_get_frame_info(mod_ctx, frame_info);
    return ESP_OK;
}

// Get module info to determine number of channels
esp_err_t mod_player_get_module_info(struct xmp_module_info *mod_info) {
    if (mod_ctx == NULL || !mod_loaded || mod_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xmp_get_module_info(mod_ctx, mod_info);
    return ESP_OK;
}

esp_err_t mod_player_toggle_channel_mute(int channel) {
    if (mod_ctx == NULL || !mod_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (channel < 0 || channel >= XMP_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // xmp_channel_mute with status=2 toggles the mute state
    int ret = xmp_channel_mute(mod_ctx, channel, 2);
    if (ret < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return ESP_OK;
}

esp_err_t mod_player_get_channel_mute(int channel, bool *is_muted) {
    if (mod_ctx == NULL || !mod_playing || is_muted == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (channel < 0 || channel >= XMP_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // xmp_channel_mute with status=-1 queries the current mute state
    int ret = xmp_channel_mute(mod_ctx, channel, -1);
    if (ret < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    *is_muted = (ret != 0);
    return ESP_OK;
}
