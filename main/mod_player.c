#include "mod_player.h"
#include "audio.h"
#include "bsp/audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// Unified backend interface (libxmp only)
#include "mod_backend.h"

// Include xmp_compat.h for structure definitions (we still need the struct definitions for compatibility)
#include "xmp_compat.h"

// Include configuration (defines MOD_CONFIG_* values)
#include "mod_backend_config.h"

static const char *TAG = "mod_player";
// -6 dB attenuation (original behavior): halve samples at I2S mix point.
static inline int16_t mod_attenuate_q15(int16_t s) {
    return (int16_t)(s >> 1);
}

// MOD player state
static mod_backend_t *mod_ctx = NULL;
static i2s_chan_handle_t i2s_handle = NULL;
static uint32_t sample_rate = MOD_CONFIG_SAMPLE_RATE;
static volatile bool mod_loaded = false;   // volatile for cross-task visibility
static volatile bool mod_playing = false;  // volatile for cross-task visibility
static volatile bool mod_paused = false;   // volatile for cross-task visibility
static TaskHandle_t mod_task_handle = NULL;
static SemaphoreHandle_t playback_mutex = NULL;  // Protects play_buffer/end_player from racing

// Task stack in PSRAM (much larger than SRAM allows)
#define MOD_TASK_STACK_SIZE MOD_CONFIG_TASK_STACK_SIZE
static StackType_t *mod_task_stack = NULL;
static StaticTask_t *mod_task_tcb = NULL;

// Audio buffer for MOD playback
#define MOD_BUFFER_SAMPLES MOD_CONFIG_BUFFER_SAMPLES
#define MOD_BUFFER_SIZE (MOD_BUFFER_SAMPLES * 2 * sizeof(int16_t))  // Stereo 16-bit

// Audio buffers - render in PSRAM, DMA buffer in internal SRAM
static int16_t *render_buffer = NULL;  // PSRAM, stereo interleaved
static DRAM_ATTR int16_t dma_buffer[MOD_BUFFER_SAMPLES * 2] = {0};  // Internal SRAM for DMA

// Task-local state that needs to be reset for each new song
static volatile bool mod_task_first_fill = true;

/**
 * @brief Reset playback task state for a new song
 * Called from mod_player_start() to ensure clean state for each playback
 */
void mod_player_reset_task_state(void) {
    mod_task_first_fill = true;
}

/**
 * @brief MOD playback task - renders MOD audio and writes to I2S
 */
static void mod_playback_task(void *arg) {
    size_t bytes_written;
    uint32_t buffer_count = 0;
    // uint32_t last_log_time = 0;  // Commented out - logging disabled

    // ESP_LOGI(TAG, "MOD playback task started");  // Commented out - logging disabled

    while (1) {
        // Only play if MOD is loaded, playing flag is set, not paused, and we have valid handles
        if (mod_playing && !mod_paused && mod_loaded && mod_ctx && i2s_handle) {
            // Acquire mutex before calling play_buffer to prevent race with stop()
            if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Re-check mod_playing and mod_paused after acquiring mutex (stop() may have set it false while we waited)
                int rc = -1;
                if (mod_playing && !mod_paused) {
                    // Render MOD audio in stereo (interleaved L/R pairs)
                    // Buffer size is stereo samples * 2 channels * sizeof(int16_t)
                    rc = mod_backend_play_buffer(mod_ctx, render_buffer, MOD_BUFFER_SAMPLES * 2 * sizeof(int16_t), MOD_CONFIG_DEFAULT_LOOP);
                }
                xSemaphoreGive(playback_mutex);

                if (rc == 0) {
                // Process stereo render buffer to DMA buffer
                // Check output mode: stereo for headphones, mono (downmixed) for speakers
                bool stereo_output = audio_is_stereo_output();

                if (stereo_output) {
                    // Headphones: copy stereo with attenuation and soft clip
                    for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                        int16_t left = render_buffer[i * 2 + 0];
                        int16_t right = render_buffer[i * 2 + 1];
                        dma_buffer[i * 2 + 0] = audio_soft_clip(mod_attenuate_q15(left));
                        dma_buffer[i * 2 + 1] = audio_soft_clip(mod_attenuate_q15(right));
                    }
                } else {
                    // Speakers: downmix stereo to mono, then duplicate for I2S
                    for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                        int16_t left = render_buffer[i * 2 + 0];
                        int16_t right = render_buffer[i * 2 + 1];
                        // Downmix: average L+R (with headroom to prevent clipping)
                        int32_t mono = ((int32_t)left + (int32_t)right) / 2;
                        int16_t sample = audio_soft_clip(mod_attenuate_q15((int16_t)mono));
                        dma_buffer[i * 2 + 0] = sample;
                        dma_buffer[i * 2 + 1] = sample;
                    }
                }

                // Pre-fill I2S buffer with a few buffers to ensure continuous playback
                // (I2S DMA might need multiple buffers to start output)
                if (mod_task_first_fill) {
                    mod_task_first_fill = false;
                    // Write 4 buffers quickly to fill DMA buffer
                    for (int j = 0; j < 4; j++) {
                        size_t temp_written = 0;
                        i2s_channel_write(i2s_handle, dma_buffer, sizeof(dma_buffer),
                                         &temp_written, 0);  // Non-blocking for quick fill
                        if (temp_written < sizeof(dma_buffer)) {
                            break;  // Buffer full, stop pre-filling
                        }
                        // Get next buffer from backend for pre-fill (with mutex protection)
                        int pre_rc = -1;
                        if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            if (mod_playing) {
                                pre_rc = mod_backend_play_buffer(mod_ctx, render_buffer, MOD_BUFFER_SAMPLES * 2 * sizeof(int16_t), MOD_CONFIG_DEFAULT_LOOP);
                            }
                            xSemaphoreGive(playback_mutex);
                        }
                        if (pre_rc != 0) break;
                        // Process pre-fill buffer with same stereo/mono logic
                        stereo_output = audio_is_stereo_output();
                        if (stereo_output) {
                            for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                                dma_buffer[i * 2 + 0] = audio_soft_clip(mod_attenuate_q15(render_buffer[i * 2 + 0]));
                                dma_buffer[i * 2 + 1] = audio_soft_clip(mod_attenuate_q15(render_buffer[i * 2 + 1]));
                            }
                        } else {
                            for (int i = 0; i < MOD_BUFFER_SAMPLES; i++) {
                                int32_t mono = ((int32_t)render_buffer[i * 2 + 0] + (int32_t)render_buffer[i * 2 + 1]) / 2;
                                int16_t sample = audio_soft_clip(mod_attenuate_q15((int16_t)mono));
                                dma_buffer[i * 2 + 0] = sample;
                                dma_buffer[i * 2 + 1] = sample;
                            }
                        }
                    }
                }

                // Write to I2S (blocking, same as beep - ensures continuous playback)
                // Channel is already enabled by audio_init(), no need to re-enable
                esp_err_t write_ret = i2s_channel_write(i2s_handle, dma_buffer,
                                   sizeof(dma_buffer),
                                   &bytes_written,
                                   portMAX_DELAY);  // Blocking to ensure continuous audio

                // Log first write result for debugging
                static bool mod_first_write_logged = false;
                if (!mod_first_write_logged) {
                    ESP_LOGI(TAG, "MOD first write: handle=%p, ret=%s, bytes=%zu",
                             (void *)i2s_handle, esp_err_to_name(write_ret), bytes_written);
                    mod_first_write_logged = true;
                }

                buffer_count++;
                
                // No yield needed - blocking I2S write already handles timing
                // Large buffer (4096 samples ~93ms) provides sufficient headroom
                // Logging commented out - removed ret check and all logging statements
                } else {
                    // Playback ended or error
                    // ESP_LOGI(TAG, "MOD playback ended (rc=%d) after %lu buffers", rc, buffer_count);
                    mod_playing = false;
                    buffer_count = 0;
                    audio_output_release(AUDIO_OUTPUT_OWNER_MOD);
                }
            }  // xSemaphoreTake block
        } else {
            // Not playing, write silence to prevent audio glitches
            if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_MOD)) {
                memset(dma_buffer, 0, sizeof(dma_buffer));
                i2s_channel_write(i2s_handle, dma_buffer, sizeof(dma_buffer), &bytes_written, 0);  // Non-blocking
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
    ESP_LOGI(TAG, "Got I2S handle: %p", (void *)i2s_handle);

    // Create mutex to protect play_buffer/end_player from racing
    playback_mutex = xSemaphoreCreateMutex();
    if (playback_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create playback mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create backend context
    ESP_LOGI(TAG, "Initializing MOD player with backend: %s", mod_backend_get_name());
    mod_ctx = mod_backend_create();
    if (mod_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to create MOD backend context");
        return ESP_ERR_NO_MEM;
    }

    // Allocate stereo render buffer in PSRAM to save internal RAM
    render_buffer = (int16_t *)heap_caps_malloc(MOD_BUFFER_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (render_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MOD render buffer in PSRAM");
        mod_backend_free(mod_ctx);
        mod_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate task stack in PSRAM (much larger than SRAM allows)
    // This allows us to use a 16KB stack without worrying about SRAM constraints
    mod_task_stack = (StackType_t *)heap_caps_malloc(MOD_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    if (mod_task_stack == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task stack in PSRAM");
        heap_caps_free(render_buffer);
        render_buffer = NULL;
        mod_backend_free(mod_ctx);
        mod_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate TCB (Task Control Block) - can be in SRAM as it's small
    mod_task_tcb = (StaticTask_t *)pvPortMalloc(sizeof(StaticTask_t));
    if (mod_task_tcb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task TCB");
        heap_caps_free(mod_task_stack);
        mod_task_stack = NULL;
        heap_caps_free(render_buffer);
        render_buffer = NULL;
        mod_backend_free(mod_ctx);
        mod_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create MOD playback task with static stack in PSRAM
    // Note: MOD backends need significant stack space for MOD processing
    mod_task_handle = xTaskCreateStaticPinnedToCore(
        mod_playback_task,
        "mod_player",
        MOD_TASK_STACK_SIZE,             // Stack size (16KB in PSRAM)
        NULL,                            // Parameters
        configMAX_PRIORITIES - 1,        // Highest user priority for real-time audio
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
        heap_caps_free(render_buffer);
        render_buffer = NULL;
        mod_backend_free(mod_ctx);
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

    // Stop current playback if any (also releases module and clears mod_loaded)
    if (mod_loaded) {
        mod_player_stop();
    }

    // Load MOD from memory
    esp_err_t ret = mod_backend_load_module(mod_ctx, mod_data, mod_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load MOD file");
        return ret;
    }

    mod_loaded = true;
    ESP_LOGI(TAG, "MOD file loaded (%zu bytes)", mod_size);

    // Get module info
    mod_module_info_t mod_info;
    if (mod_backend_get_module_info(mod_ctx, &mod_info) == ESP_OK) {
        if (mod_info.name && mod_info.name[0] != '\0') {
            ESP_LOGI(TAG, "Module name: %s", mod_info.name);
        }
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

    // Configure backend settings
    // Enable/disable DSP filtering based on config
    if (MOD_CONFIG_DSP_ENABLE) {
        mod_backend_set_player(mod_ctx, MOD_PLAYER_DSP, MOD_DSP_LOWPASS);
    }

    // Ensure I2S channel is enabled (may have been disabled by audio_stop())
    if (i2s_handle) {
        if (!audio_output_acquire(AUDIO_OUTPUT_OWNER_MOD)) {
            ESP_LOGW(TAG, "MOD start: audio output owned by another player");
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t rate_ret = audio_set_sample_rate(sample_rate);
        if (rate_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set MOD sample rate: %s", esp_err_to_name(rate_ret));
            audio_output_release(AUDIO_OUTPUT_OWNER_MOD);
            return rate_ret;
        }
        // audio_set_sample_rate() already re-enabled the channel
        // Reset DMA state and preload silence to avoid replaying old buffers
        audio_i2s_preload_and_enable(i2s_handle, dma_buffer, sizeof(dma_buffer));
    }

    // Start player with configured format
    esp_err_t ret = mod_backend_start_player(mod_ctx, sample_rate, MOD_CONFIG_FORMAT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MOD player");
        return ret;
    }
    
    // Configure backend settings for audio quality (interpolation mode)
    mod_backend_set_player(mod_ctx, MOD_PLAYER_INTERP, MOD_CONFIG_INTERP);
    
    #if MOD_CONFIG_VERBOSE_LOG
    const char *format_str = (MOD_CONFIG_FORMAT == MOD_CONFIG_FORMAT_MONO) ? "MONO" : "STEREO";
    const char *interp_str = "UNKNOWN";
    switch(MOD_CONFIG_INTERP) {
        case MOD_CONFIG_INTERP_NEAREST: interp_str = "NEAREST"; break;
        case MOD_CONFIG_INTERP_LINEAR: interp_str = "LINEAR"; break;
        case MOD_CONFIG_INTERP_CUBIC: interp_str = "CUBIC"; break;
        case MOD_CONFIG_INTERP_SINC: interp_str = "SINC"; break;
    }
    ESP_LOGI(TAG, "MOD player started at %lu Hz, format: %s, interpolation: %s, DSP: %s (%s)", 
             sample_rate, format_str, interp_str, 
             MOD_CONFIG_DSP_ENABLE ? "enabled" : "disabled",
             mod_backend_get_name());
    #else
    ESP_LOGI(TAG, "MOD player started at %lu Hz (%s)", sample_rate, mod_backend_get_name());
    #endif

    mod_playing = true;

    // Reset playback task state for new song
    extern void mod_player_reset_task_state(void);
    mod_player_reset_task_state();

    ESP_LOGI(TAG, "MOD playback started");
    return ESP_OK;
}

esp_err_t mod_player_stop(void) {
    if (mod_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!mod_playing && !mod_loaded) {
        return ESP_OK;
    }

    // Set flags FIRST to signal audio task to stop calling play_buffer
    mod_playing = false;
    mod_paused = false;  // Reset paused state when stopping

    // Acquire mutex to ensure audio task has finished any in-progress play_buffer call
    // This guarantees we don't call end_player while play_buffer is running
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        mod_backend_end_player(mod_ctx);
        mod_backend_release_module(mod_ctx);
        xSemaphoreGive(playback_mutex);
    } else {
        // Timeout - force end anyway (shouldn't happen normally)
        ESP_LOGW(TAG, "Timeout waiting for playback mutex in stop()");
        mod_backend_end_player(mod_ctx);
        mod_backend_release_module(mod_ctx);
    }

    // Mark as unloaded after backend cleanup
    mod_loaded = false;

    // Flush I2S with silence before releasing ownership to avoid repeating the last buffer
    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_MOD)) {
        audio_i2s_silence_and_disable(i2s_handle, dma_buffer, sizeof(dma_buffer), 3);
    }

    audio_output_release(AUDIO_OUTPUT_OWNER_MOD);

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
    if (mod_ctx == NULL || !mod_loaded || frame_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Protect against race with playback task modifying backend state
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Convert from backend frame info to libxmp frame info
    mod_frame_info_t mod_frame;
    esp_err_t ret = mod_backend_get_frame_info(mod_ctx, &mod_frame);
    if (ret != ESP_OK) {
        xSemaphoreGive(playback_mutex);
        return ret;
    }
    
    // Map backend frame info to libxmp format
    frame_info->pos = mod_frame.pos;
    frame_info->pattern = mod_frame.pattern;
    frame_info->row = mod_frame.row;
    frame_info->speed = mod_frame.speed;
    frame_info->bpm = mod_frame.bpm;
    frame_info->frame_time = mod_frame.frame_time;
    frame_info->num_channels = mod_frame.num_channels;
    
    // Copy channel info - both use event structure
    int max_channels = (mod_frame.num_channels < XMP_MAX_CHANNELS) ? mod_frame.num_channels : XMP_MAX_CHANNELS;
    for (int ch = 0; ch < max_channels; ch++) {
        frame_info->channel_info[ch].event.note = mod_frame.channel_info[ch].event.note;
        frame_info->channel_info[ch].event.ins = mod_frame.channel_info[ch].event.ins;
        frame_info->channel_info[ch].event.fxt = mod_frame.channel_info[ch].event.fxt;
        frame_info->channel_info[ch].event.fxp = mod_frame.channel_info[ch].event.fxp;
        frame_info->channel_info[ch].volume = mod_frame.channel_info[ch].volume;
        frame_info->channel_info[ch].period = mod_frame.channel_info[ch].period;
    }

    xSemaphoreGive(playback_mutex);
    return ESP_OK;
}

// Static storage for module info (persists across calls)
// This matches libxmp's xmp_module structure
static struct xmp_module static_mod_data = {0};

// Get module info to determine number of channels
esp_err_t mod_player_get_module_info(struct xmp_module_info *mod_info) {
    if (mod_ctx == NULL || !mod_loaded || mod_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Convert from backend module info to libxmp module info
    mod_module_info_t mod_info_data;
    esp_err_t ret = mod_backend_get_module_info(mod_ctx, &mod_info_data);
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Map backend module info to libxmp format
    // libxmp uses mod_info->mod->name, mod_info->mod->chn, etc.
    // We need to populate static_mod_data and point mod_info->mod to it
    if (mod_info_data.name != NULL) {
        strncpy(static_mod_data.name, mod_info_data.name, sizeof(static_mod_data.name) - 1);
        static_mod_data.name[sizeof(static_mod_data.name) - 1] = '\0';
    } else {
        static_mod_data.name[0] = '\0';
    }
    static_mod_data.chn = mod_info_data.chn;
    static_mod_data.pat = mod_info_data.pat;
    
    // Point mod_info->mod to our static data
    mod_info->mod = &static_mod_data;
    
    return ESP_OK;
}

esp_err_t mod_player_toggle_channel_mute(int channel) {
    if (mod_ctx == NULL || !mod_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (channel < 0 || channel >= MOD_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // mod_backend_channel_mute with status=2 toggles the mute state
    int ret = mod_backend_channel_mute(mod_ctx, channel, 2);
    if (ret < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return ESP_OK;
}

esp_err_t mod_player_get_channel_mute(int channel, bool *is_muted) {
    if (mod_ctx == NULL || !mod_playing || is_muted == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (channel < 0 || channel >= MOD_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // mod_backend_channel_mute with status=-1 queries the current mute state
    int ret = mod_backend_channel_mute(mod_ctx, channel, -1);
    if (ret < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    
    *is_muted = (ret != 0);
    return ESP_OK;
}

esp_err_t mod_player_get_pattern_row_channel(int pattern, int row, int channel, 
                                              uint8_t *note, uint8_t *ins, 
                                              uint8_t *fxt, uint8_t *fxp) {
    if (mod_ctx == NULL || !mod_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return mod_backend_get_pattern_row_channel(mod_ctx, pattern, row, channel, note, ins, fxt, fxp);
}

esp_err_t mod_player_get_pattern_num_rows(int pattern, int *num_rows) {
    if (mod_ctx == NULL || !mod_loaded || num_rows == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    return mod_backend_get_pattern_num_rows(mod_ctx, pattern, num_rows);
}

esp_err_t mod_player_get_order_pattern(int order, int *pattern) {
    if (mod_ctx == NULL || !mod_loaded || pattern == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return mod_backend_get_order_pattern(mod_ctx, order, pattern);
}

esp_err_t mod_player_pause(void) {
    if (!mod_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    mod_paused = true;
    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_MOD)) {
        audio_i2s_silence_and_disable(i2s_handle, dma_buffer, sizeof(dma_buffer), 2);
    }
    return ESP_OK;
}

esp_err_t mod_player_resume(void) {
    if (!mod_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_MOD)) {
        esp_err_t rate_ret = audio_set_sample_rate(sample_rate);
        if (rate_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set MOD sample rate: %s", esp_err_to_name(rate_ret));
            return rate_ret;
        }
        audio_i2s_preload_and_enable(i2s_handle, dma_buffer, sizeof(dma_buffer));
    }
    mod_paused = false;
    return ESP_OK;
}

bool mod_player_is_paused(void) {
    return mod_paused;
}
