/**
 * @file vgm_player.c
 * @brief VGM/VGZ player implementation
 *
 * This module provides high-level VGM playback following the mod_player pattern.
 * Uses FreeRTOS task on Core 1 with max priority for real-time audio.
 */

#include "vgm_player.h"
#include "vgm_backend.h"
#include "audio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "vgm_player";

// VGM player state
static vgm_backend_t *vgm_ctx = NULL;
static i2s_chan_handle_t i2s_handle = NULL;
static uint32_t sample_rate = VGM_PLAYBACK_SAMPLE_RATE;
static volatile bool vgm_loaded = false;
static volatile bool vgm_playing = false;
static volatile bool vgm_paused = false;
static TaskHandle_t vgm_task_handle = NULL;
static SemaphoreHandle_t playback_mutex = NULL;
static volatile uint32_t vgm_skip_samples = 0;
static volatile bool vgm_skip_muted = false;
static char vgm_last_error[128];

// Task stack in PSRAM
#define VGM_TASK_STACK_SIZE 16384
static StackType_t *vgm_task_stack = NULL;
static StaticTask_t *vgm_task_tcb = NULL;

// Audio buffer for VGM playback
#define VGM_BUFFER_SAMPLES 2048
#define VGM_BUFFER_SIZE (VGM_BUFFER_SAMPLES * 2 * sizeof(int16_t))  // Stereo 16-bit
#define VGM_DMA_CHUNK_SAMPLES 1024  // DMA buffer in internal RAM
#define VGM_SKIP_CHUNKS_PER_LOOP 4

// Waveform capture for oscilloscope visualization
#define VGM_WAVEFORM_SAMPLES 512  // Samples to capture for display
static int16_t vgm_waveform_left[VGM_WAVEFORM_SAMPLES];
static int16_t vgm_waveform_right[VGM_WAVEFORM_SAMPLES];
static volatile bool vgm_waveform_ready = false;
static portMUX_TYPE waveform_lock = portMUX_INITIALIZER_UNLOCKED;

// Audio buffers: render in PSRAM to save internal RAM, DMA in internal SRAM
static int16_t *vgm_render_buffer = NULL;  // PSRAM
static int16_t *vgm_dma_buffer = NULL;     // Internal SRAM, DMA-capable

static void vgm_player_set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(vgm_last_error, sizeof(vgm_last_error), fmt, args);
    va_end(args);
}

static bool vgm_player_chip_unknown(const char *name) {
    if (!name || !name[0]) {
        return true;
    }
    return (strcasecmp(name, "unknown") == 0);
}

static esp_err_t vgm_player_check_supported(void) {
    vgm_playback_info_t info = {0};
    if (vgm_backend_get_info(vgm_ctx, &info) != ESP_OK) {
        return ESP_OK;
    }
    if (info.num_chips == 0) {
        vgm_player_set_error("No VGM chips detected");
        return ESP_ERR_NOT_SUPPORTED;
    }

    char unsupported[96] = {0};
    for (uint8_t i = 0; i < info.num_chips; i++) {
        vgm_chip_info_t chip = {0};
        if (vgm_backend_get_chip_info(vgm_ctx, i, &chip) != ESP_OK || vgm_player_chip_unknown(chip.name)) {
            const char *name = (chip.name && chip.name[0]) ? chip.name : "Unknown";
            if (unsupported[0] != '\0') {
                strncat(unsupported, ", ", sizeof(unsupported) - strlen(unsupported) - 1);
            }
            strncat(unsupported, name, sizeof(unsupported) - strlen(unsupported) - 1);
        }
    }

    if (unsupported[0] != '\0') {
        vgm_player_set_error("Unsupported VGM chip(s): %s", unsupported);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

// Task-local state that needs to be reset for each new song
static volatile bool vgm_task_first_write_logged = false;
static volatile uint32_t vgm_task_enable_fail_count = 0;

/**
 * @brief Reset playback task state for a new song
 * Called from vgm_player_start() to ensure clean state for each playback
 */
void vgm_player_reset_task_state(void) {
    vgm_task_first_write_logged = false;
    vgm_task_enable_fail_count = 0;
}

/**
 * @brief VGM playback task - renders VGM audio and writes to I2S
 */
static void vgm_playback_task(void *arg) {
    size_t bytes_written;
    int64_t last_stats_time = 0;
    uint32_t stat_render_max_us = 0;
    uint32_t stat_copy_max_us = 0;
    uint32_t stat_write_max_us = 0;
    uint32_t stat_loop_max_us = 0;

    while (1) {
        int64_t loop_start_us = esp_timer_get_time();
        // Only play if VGM is loaded, playing flag is set, and not paused
        if (vgm_playing && vgm_loaded && vgm_ctx && i2s_handle) {
            // Acquire mutex before rendering to prevent race with stop()
            if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint32_t rendered = 0;

                // Re-check state after acquiring mutex
                if (vgm_playing && !vgm_paused) {
                    if (vgm_skip_samples > 0) {
                        if (!vgm_skip_muted && i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
                            // Mute output during skip to avoid squawks
                            if (vgm_dma_buffer) {
                                audio_i2s_silence_and_disable(i2s_handle, vgm_dma_buffer,
                                                              VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t), 2);
                            }
                            vgm_skip_muted = true;
                        }

                        bool ended = false;
                        for (int i = 0; i < VGM_SKIP_CHUNKS_PER_LOOP && vgm_skip_samples > 0; i++) {
                            uint32_t chunk = (vgm_skip_samples > VGM_BUFFER_SAMPLES) ? VGM_BUFFER_SAMPLES : vgm_skip_samples;
                            uint32_t skipped = vgm_backend_render(vgm_ctx, vgm_render_buffer, chunk);
                            if (skipped == 0) {
                                ended = true;
                                vgm_skip_samples = 0;
                                break;
                            }
                            if (skipped > vgm_skip_samples) {
                                vgm_skip_samples = 0;
                            } else {
                                vgm_skip_samples -= skipped;
                            }
                        }

                        if (ended) {
                            xSemaphoreGive(playback_mutex);
                            vgm_player_stop();
                            continue;
                        }

                        if (vgm_skip_samples == 0 && vgm_skip_muted) {
                            if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
                                esp_err_t rate_ret = audio_set_sample_rate(sample_rate);
                                if (rate_ret != ESP_OK) {
                                    ESP_LOGE(TAG, "Failed to set VGM sample rate after skip: %s",
                                             esp_err_to_name(rate_ret));
                                }
                                if (vgm_dma_buffer) {
                                    audio_i2s_preload_and_enable(i2s_handle, vgm_dma_buffer,
                                                                 VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t));
                                }
                            }
                            vgm_skip_muted = false;
                        }
                        xSemaphoreGive(playback_mutex);
                        vTaskDelay(pdMS_TO_TICKS(1));
                        continue;
                    }

                    // Render VGM audio (stereo interleaved 16-bit)
                    int64_t render_start_us = esp_timer_get_time();
                    rendered = vgm_backend_render(vgm_ctx, vgm_render_buffer, VGM_BUFFER_SAMPLES);
                    uint32_t render_us = (uint32_t)(esp_timer_get_time() - render_start_us);
                    if (render_us > stat_render_max_us) stat_render_max_us = render_us;

                    // Capture waveform for visualization (decimate to fit display)
                    if (rendered >= VGM_WAVEFORM_SAMPLES) {
                        taskENTER_CRITICAL(&waveform_lock);
                        int step = rendered / VGM_WAVEFORM_SAMPLES;
                        for (int i = 0; i < VGM_WAVEFORM_SAMPLES; i++) {
                            int src_idx = i * step * 2;  // Stereo interleaved
                            vgm_waveform_left[i] = vgm_render_buffer[src_idx];
                            vgm_waveform_right[i] = vgm_render_buffer[src_idx + 1];
                        }
                        vgm_waveform_ready = true;
                        taskEXIT_CRITICAL(&waveform_lock);
                    }
                }
                xSemaphoreGive(playback_mutex);

                if (rendered > 0) {
                    if (!vgm_task_first_write_logged) {
                        // Try to enable channel right before first write
                        esp_err_t en_ret = i2s_channel_enable(i2s_handle);
                        if (en_ret == ESP_OK || en_ret == ESP_ERR_INVALID_STATE) {
                            // INVALID_STATE can mean already enabled; treat as OK for first write
                            ESP_LOGI(TAG, "VGM TASK: handle=%p, enable=%s(0x%x), rendered=%lu",
                                     (void *)i2s_handle, esp_err_to_name(en_ret), en_ret, rendered);
                            vgm_task_first_write_logged = true;
                            vgm_task_enable_fail_count = 0;
                        } else {
                            if (vgm_task_enable_fail_count < 3) {
                                ESP_LOGE(TAG, "VGM TASK: enable failed: %s(0x%x), handle=%p",
                                         esp_err_to_name(en_ret), en_ret, (void *)i2s_handle);
                            }
                            vgm_task_enable_fail_count++;
                            vTaskDelay(pdMS_TO_TICKS(10));
                            continue;  // Skip write until channel is enabled
                        }
                    }
                    // Write to I2S in DMA-sized chunks (render buffer lives in PSRAM)
                    uint32_t remaining = rendered;
                    uint32_t offset = 0;
                    bool stereo_output = audio_is_stereo_output();
                    while (remaining > 0) {
                        uint32_t chunk = (remaining > VGM_DMA_CHUNK_SAMPLES) ? VGM_DMA_CHUNK_SAMPLES : remaining;
                        int64_t copy_start_us = esp_timer_get_time();
                        // Process audio: stereo for headphones, mono downmix for speakers
                        int16_t *src = &vgm_render_buffer[offset * 2];
                        if (stereo_output) {
                            // Headphones: copy stereo with soft clipping
                            for (uint32_t i = 0; i < chunk; i++) {
                                vgm_dma_buffer[i * 2 + 0] = audio_soft_clip(src[i * 2 + 0]);
                                vgm_dma_buffer[i * 2 + 1] = audio_soft_clip(src[i * 2 + 1]);
                            }
                        } else {
                            // Speakers: downmix to mono, duplicate for I2S
                            for (uint32_t i = 0; i < chunk; i++) {
                                int32_t mono = ((int32_t)src[i * 2 + 0] + (int32_t)src[i * 2 + 1]) / 2;
                                int16_t sample = audio_soft_clip((int16_t)mono);
                                vgm_dma_buffer[i * 2 + 0] = sample;
                                vgm_dma_buffer[i * 2 + 1] = sample;
                            }
                        }
                        uint32_t copy_us = (uint32_t)(esp_timer_get_time() - copy_start_us);
                        if (copy_us > stat_copy_max_us) stat_copy_max_us = copy_us;
                        int64_t write_start_us = esp_timer_get_time();
                        esp_err_t write_ret = i2s_channel_write(i2s_handle, vgm_dma_buffer,
                                          chunk * 2 * sizeof(int16_t),
                                          &bytes_written, portMAX_DELAY);
                        uint32_t write_us = (uint32_t)(esp_timer_get_time() - write_start_us);
                        if (write_us > stat_write_max_us) stat_write_max_us = write_us;
                        if (write_ret != ESP_OK) {
                            // Log write errors (limited) and retry enabling on next loop
                            static int error_count = 0;
                            if (error_count < 3) {
                                ESP_LOGE(TAG, "write failed: %s, handle=%p",
                                         esp_err_to_name(write_ret), (void *)i2s_handle);
                                error_count++;
                            }
                            vgm_task_first_write_logged = false;
                            vTaskDelay(pdMS_TO_TICKS(10));
                            break;
                        }
                        remaining -= chunk;
                        offset += chunk;
                    }
                } else {
                    // Playback ended
                    vgm_playing = false;
                    vgm_task_first_write_logged = false;  // Reset for next playback
                    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
                        i2s_channel_disable(i2s_handle);
                    }
                    audio_output_release(AUDIO_OUTPUT_OWNER_VGM);
                }
            }
        } else {
            // Not playing - don't write silence (causes errors if channel not enabled)
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        uint32_t loop_us = (uint32_t)(esp_timer_get_time() - loop_start_us);
        if (loop_us > stat_loop_max_us) stat_loop_max_us = loop_us;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_stats_time > 1000000) {
            ESP_LOGI(TAG, "VGM timing us (max): render=%u copy=%u write=%u loop=%u",
                     stat_render_max_us, stat_copy_max_us, stat_write_max_us, stat_loop_max_us);
            stat_render_max_us = 0;
            stat_copy_max_us = 0;
            stat_write_max_us = 0;
            stat_loop_max_us = 0;
            last_stats_time = now_us;
        }
    }
}

esp_err_t vgm_player_init(uint32_t sample_rate_in) {
    if (vgm_ctx != NULL) {
        ESP_LOGW(TAG, "VGM player already initialized");
        return ESP_OK;
    }

    sample_rate = sample_rate_in;

    // Get I2S handle from audio system
    esp_err_t ret = audio_get_i2s_handle((void **)&i2s_handle);
    if (ret != ESP_OK || i2s_handle == NULL) {
        ESP_LOGE(TAG, "I2S handle not available - call audio_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Got I2S handle: %p", (void *)i2s_handle);

    // Create mutex for thread safety
    playback_mutex = xSemaphoreCreateMutex();
    if (playback_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create playback mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create VGM backend
    vgm_ctx = vgm_backend_create();
    if (vgm_ctx == NULL) {
        ESP_LOGE(TAG, "Failed to create VGM backend");
        vSemaphoreDelete(playback_mutex);
        playback_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate task stack in PSRAM
    vgm_task_stack = (StackType_t *)heap_caps_malloc(VGM_TASK_STACK_SIZE * sizeof(StackType_t),
                                                      MALLOC_CAP_SPIRAM);
    if (vgm_task_stack == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task stack in PSRAM");
        vgm_backend_free(vgm_ctx);
        vgm_ctx = NULL;
        vSemaphoreDelete(playback_mutex);
        playback_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate TCB
    vgm_task_tcb = (StaticTask_t *)pvPortMalloc(sizeof(StaticTask_t));
    if (vgm_task_tcb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task TCB");
        heap_caps_free(vgm_task_stack);
        vgm_task_stack = NULL;
        vgm_backend_free(vgm_ctx);
        vgm_ctx = NULL;
        vSemaphoreDelete(playback_mutex);
        playback_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate render buffer in PSRAM to save internal RAM
    vgm_render_buffer = (int16_t *)heap_caps_malloc(VGM_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (vgm_render_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate VGM render buffer in PSRAM");
        heap_caps_free(vgm_task_stack);
        vgm_task_stack = NULL;
        vPortFree(vgm_task_tcb);
        vgm_task_tcb = NULL;
        vgm_backend_free(vgm_ctx);
        vgm_ctx = NULL;
        vSemaphoreDelete(playback_mutex);
        playback_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Allocate DMA buffer in internal SRAM
    vgm_dma_buffer = (int16_t *)heap_caps_malloc(VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (vgm_dma_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate VGM DMA buffer in internal RAM");
        heap_caps_free(vgm_render_buffer);
        vgm_render_buffer = NULL;
        heap_caps_free(vgm_task_stack);
        vgm_task_stack = NULL;
        vPortFree(vgm_task_tcb);
        vgm_task_tcb = NULL;
        vgm_backend_free(vgm_ctx);
        vgm_ctx = NULL;
        vSemaphoreDelete(playback_mutex);
        playback_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Create VGM playback task on Core 1
    vgm_task_handle = xTaskCreateStaticPinnedToCore(
        vgm_playback_task,
        "vgm_player",
        VGM_TASK_STACK_SIZE,
        NULL,
        configMAX_PRIORITIES - 1,  // Highest user priority
        vgm_task_stack,
        vgm_task_tcb,
        1  // Core 1
    );

    if (vgm_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create VGM playback task");
        heap_caps_free(vgm_task_stack);
        vgm_task_stack = NULL;
        vPortFree(vgm_task_tcb);
        vgm_task_tcb = NULL;
        vgm_backend_free(vgm_ctx);
        vgm_ctx = NULL;
        vSemaphoreDelete(playback_mutex);
        playback_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "VGM player initialized (sample rate: %lu Hz)", sample_rate);
    return ESP_OK;
}

esp_err_t vgm_player_load(const uint8_t *vgm_data, size_t vgm_size) {
    if (vgm_ctx == NULL) {
        ESP_LOGE(TAG, "VGM player not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop current playback if any
    if (vgm_loaded) {
        vgm_player_stop();
    }

    // Load VGM file
    esp_err_t ret = vgm_backend_load(vgm_ctx, vgm_data, vgm_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load VGM file");
        return ret;
    }

    vgm_loaded = true;
    ESP_LOGI(TAG, "VGM file loaded (%zu bytes)", vgm_size);

    // Log GD3 tags if available
    vgm_tags_t tags;
    if (vgm_backend_get_tags(vgm_ctx, &tags) == ESP_OK) {
        if (tags.title[0]) {
            ESP_LOGI(TAG, "Title: %s", tags.title);
        }
        if (tags.game[0]) {
            ESP_LOGI(TAG, "Game: %s", tags.game);
        }
        if (tags.system[0]) {
            ESP_LOGI(TAG, "System: %s", tags.system);
        }
        if (tags.author[0]) {
            ESP_LOGI(TAG, "Author: %s", tags.author);
        }
    }

    return ESP_OK;
}

esp_err_t vgm_player_start(void) {
    if (vgm_ctx == NULL || !vgm_loaded) {
        ESP_LOGE(TAG, "VGM not loaded");
        return ESP_ERR_INVALID_STATE;
    }

    if (vgm_playing) {
        ESP_LOGW(TAG, "VGM already playing");
        return ESP_OK;
    }

    vgm_last_error[0] = '\0';
    esp_err_t support_ret = vgm_player_check_supported();
    if (support_ret != ESP_OK) {
        return support_ret;
    }

    // Re-get I2S handle fresh to ensure we have the current one
    i2s_chan_handle_t fresh_handle = NULL;
    esp_err_t handle_ret = audio_get_i2s_handle((void **)&fresh_handle);
    ESP_LOGW(TAG, "vgm_player_start: stored=%p, fresh=%p, ret=%s",
             (void *)i2s_handle, (void *)fresh_handle, esp_err_to_name(handle_ret));

    // Use fresh handle if different
    if (handle_ret == ESP_OK && fresh_handle != NULL) {
        if (fresh_handle != i2s_handle) {
            ESP_LOGW(TAG, "I2S handle changed! Updating from %p to %p",
                     (void *)i2s_handle, (void *)fresh_handle);
            i2s_handle = fresh_handle;
        }
    }

    // Ensure I2S channel is enabled (may have been disabled by audio_stop())
    if (i2s_handle) {
        if (!audio_output_acquire(AUDIO_OUTPUT_OWNER_VGM)) {
            ESP_LOGW(TAG, "VGM start: audio output owned by another player");
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t rate_ret = audio_set_sample_rate(sample_rate);
        if (rate_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set VGM sample rate: %s", esp_err_to_name(rate_ret));
            audio_output_release(AUDIO_OUTPUT_OWNER_VGM);
            return rate_ret;
        }
        // audio_set_sample_rate() already re-enabled the channel
        // Reset DMA state and preload silence to avoid replaying old buffers
        if (vgm_dma_buffer) {
            audio_i2s_preload_and_enable(i2s_handle, vgm_dma_buffer,
                                         VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t));
        }
    } else {
        ESP_LOGE(TAG, "i2s_handle is NULL!");
        return ESP_ERR_INVALID_STATE;
    }

    // Start backend playback
    esp_err_t ret = vgm_backend_start(vgm_ctx, sample_rate);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start VGM playback");
        return ret;
    }

    vgm_playing = true;
    vgm_paused = false;

    // Reset playback task state for new song
    extern void vgm_player_reset_task_state(void);
    vgm_player_reset_task_state();

    ESP_LOGI(TAG, "VGM playback started");
    return ESP_OK;
}

esp_err_t vgm_player_stop(void) {
    if (vgm_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!vgm_playing && !vgm_loaded) {
        return ESP_OK;
    }

    // Set flags first to signal task to stop
    vgm_playing = false;
    vgm_paused = false;

    // Acquire mutex to ensure task has finished any in-progress render
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        vgm_backend_stop(vgm_ctx);
        vgm_backend_unload(vgm_ctx);
        xSemaphoreGive(playback_mutex);
    } else {
        ESP_LOGW(TAG, "Timeout waiting for playback mutex in stop()");
        vgm_backend_stop(vgm_ctx);
        vgm_backend_unload(vgm_ctx);
    }

    // Mark as unloaded after backend cleanup
    vgm_loaded = false;

    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
        // Flush DMA with silence before disabling to avoid repeating the last buffer
        if (vgm_dma_buffer) {
            audio_i2s_silence_and_disable(i2s_handle, vgm_dma_buffer,
                                          VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t), 3);
        }
    }
    audio_output_release(AUDIO_OUTPUT_OWNER_VGM);

    ESP_LOGI(TAG, "VGM playback stopped");
    return ESP_OK;
}

esp_err_t vgm_player_pause(void) {
    if (!vgm_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    vgm_paused = true;
    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
        // Flush DMA with silence before disabling to avoid repeating the last buffer
        if (vgm_dma_buffer) {
            audio_i2s_silence_and_disable(i2s_handle, vgm_dma_buffer,
                                          VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t), 2);
        }
    }
    return ESP_OK;
}

esp_err_t vgm_player_resume(void) {
    if (!vgm_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
        esp_err_t rate_ret = audio_set_sample_rate(sample_rate);
        if (rate_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set VGM sample rate: %s", esp_err_to_name(rate_ret));
            return rate_ret;
        }
        if (vgm_dma_buffer) {
            audio_i2s_preload_and_enable(i2s_handle, vgm_dma_buffer,
                                         VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t));
        }
    }
    vgm_paused = false;
    return ESP_OK;
}

esp_err_t vgm_player_skip_seconds(uint32_t seconds) {
    if (!vgm_playing || !vgm_loaded || vgm_ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (seconds == 0) {
        return ESP_OK;
    }

    vgm_playback_info_t info = {0};
    uint64_t remaining = (uint64_t)seconds * (uint64_t)sample_rate;
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (vgm_backend_get_info(vgm_ctx, &info) == ESP_OK && info.total_samples > 0) {
        if (info.current_sample < info.total_samples) {
            uint32_t max_skip = info.total_samples - info.current_sample;
            if (remaining > max_skip) {
                remaining = max_skip;
            }
        } else {
            remaining = 0;
        }
    }

    if (remaining > 0 && info.total_samples > 0) {
        uint32_t target = info.current_sample + (uint32_t)remaining;
        if (target > info.total_samples) {
            target = info.total_samples;
        }

        // Mute output during seek to avoid squawks
        if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
            if (vgm_dma_buffer) {
                audio_i2s_silence_and_disable(i2s_handle, vgm_dma_buffer,
                                              VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t), 2);
            }
        }

        if (vgm_backend_seek(vgm_ctx, target) == ESP_OK) {
            vgm_skip_samples = 0;
            vgm_skip_muted = false;
            if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
                if (vgm_dma_buffer) {
                    audio_i2s_preload_and_enable(i2s_handle, vgm_dma_buffer,
                                                 VGM_DMA_CHUNK_SAMPLES * 2 * sizeof(int16_t));
                }
            }
            xSemaphoreGive(playback_mutex);
            return ESP_OK;
        }
    }

    if (remaining > 0) {
        uint64_t pending = vgm_skip_samples + remaining;
        if (info.total_samples > 0 && info.current_sample < info.total_samples) {
            uint64_t max_total = info.total_samples - info.current_sample;
            if (pending > max_total) {
                pending = max_total;
            }
        }
        vgm_skip_samples = (uint32_t)pending;
        vgm_skip_muted = false;
    }
    xSemaphoreGive(playback_mutex);
    return ESP_OK;
}

bool vgm_player_is_playing(void) {
    return vgm_playing && vgm_loaded;
}

bool vgm_player_is_paused(void) {
    return vgm_paused;
}

bool vgm_player_is_loaded(void) {
    return vgm_loaded;
}

const char *vgm_player_get_last_error(void) {
    return (vgm_last_error[0] != '\0') ? vgm_last_error : NULL;
}

TaskHandle_t vgm_player_get_task_handle(void) {
    return vgm_task_handle;
}

esp_err_t vgm_player_get_tags(vgm_tags_t *tags) {
    if (vgm_ctx == NULL || !vgm_loaded || tags == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Protect against race with playback task modifying backend state
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = vgm_backend_get_tags(vgm_ctx, tags);
    xSemaphoreGive(playback_mutex);
    return ret;
}

esp_err_t vgm_player_get_info(vgm_playback_info_t *info) {
    if (vgm_ctx == NULL || info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Protect against race with playback task modifying backend state
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = vgm_backend_get_info(vgm_ctx, info);
    xSemaphoreGive(playback_mutex);
    return ret;
}

esp_err_t vgm_player_get_chip_info(uint8_t index, vgm_chip_info_t *chip_info) {
    if (vgm_ctx == NULL || !vgm_loaded || chip_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Protect against race with playback task modifying backend state
    if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = vgm_backend_get_chip_info(vgm_ctx, index, chip_info);
    xSemaphoreGive(playback_mutex);
    return ret;
}

void vgm_player_set_loop_count(uint32_t loops) {
    if (vgm_ctx) {
        vgm_backend_set_loop_count(vgm_ctx, loops);
    }
}

void vgm_player_set_fade_time(uint32_t fade_ms) {
    if (vgm_ctx) {
        vgm_backend_set_fade_time(vgm_ctx, fade_ms);
    }
}

esp_err_t vgm_player_get_waveform(int16_t *left, int16_t *right, size_t max_samples, size_t *out_samples) {
    if (!left || !right || !out_samples || max_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!vgm_waveform_ready) {
        *out_samples = 0;
        return ESP_ERR_NOT_FOUND;
    }

    size_t copy_samples = (max_samples < VGM_WAVEFORM_SAMPLES) ? max_samples : VGM_WAVEFORM_SAMPLES;

    taskENTER_CRITICAL(&waveform_lock);
    memcpy(left, vgm_waveform_left, copy_samples * sizeof(int16_t));
    memcpy(right, vgm_waveform_right, copy_samples * sizeof(int16_t));
    taskEXIT_CRITICAL(&waveform_lock);

    *out_samples = copy_samples;
    return ESP_OK;
}
