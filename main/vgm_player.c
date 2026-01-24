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
#include <string.h>

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

// Task stack in PSRAM
#define VGM_TASK_STACK_SIZE 16384
static StackType_t *vgm_task_stack = NULL;
static StaticTask_t *vgm_task_tcb = NULL;

// Audio buffer for VGM playback
#define VGM_BUFFER_SAMPLES 2048
#define VGM_BUFFER_SIZE (VGM_BUFFER_SAMPLES * 2 * sizeof(int16_t))  // Stereo 16-bit
#define VGM_DMA_CHUNK_SAMPLES 1024  // DMA buffer in internal RAM

// Waveform capture for oscilloscope visualization
#define VGM_WAVEFORM_SAMPLES 512  // Samples to capture for display
static int16_t vgm_waveform_left[VGM_WAVEFORM_SAMPLES];
static int16_t vgm_waveform_right[VGM_WAVEFORM_SAMPLES];
static volatile bool vgm_waveform_ready = false;
static portMUX_TYPE waveform_lock = portMUX_INITIALIZER_UNLOCKED;

// Audio buffers: render in PSRAM to save internal RAM, DMA in internal SRAM
static int16_t *vgm_render_buffer = NULL;  // PSRAM
static int16_t *vgm_dma_buffer = NULL;     // Internal SRAM, DMA-capable

/**
 * @brief VGM playback task - renders VGM audio and writes to I2S
 */
static void vgm_playback_task(void *arg) {
    size_t bytes_written;
    static bool first_write_logged = false;
    static uint32_t enable_fail_count = 0;
    int64_t last_stats_time = 0;
    uint32_t stat_render_max_us = 0;
    uint32_t stat_copy_max_us = 0;
    uint32_t stat_write_max_us = 0;
    uint32_t stat_loop_max_us = 0;

    while (1) {
        int64_t loop_start_us = esp_timer_get_time();
        // Only play if VGM is loaded, playing flag is set, and not paused
        if (vgm_playing && !vgm_paused && vgm_loaded && vgm_ctx && i2s_handle) {
            // Acquire mutex before rendering to prevent race with stop()
            if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint32_t rendered = 0;

                // Re-check state after acquiring mutex
                if (vgm_playing && !vgm_paused) {
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
                    if (!first_write_logged) {
                        // Try to enable channel right before first write
                        esp_err_t en_ret = i2s_channel_enable(i2s_handle);
                        if (en_ret == ESP_OK || en_ret == ESP_ERR_INVALID_STATE) {
                            // INVALID_STATE can mean already enabled; treat as OK for first write
                            ESP_LOGI(TAG, "VGM TASK: handle=%p, enable=%s(0x%x), rendered=%lu",
                                     (void *)i2s_handle, esp_err_to_name(en_ret), en_ret, rendered);
                            first_write_logged = true;
                            enable_fail_count = 0;
                        } else {
                            if (enable_fail_count < 3) {
                                ESP_LOGE(TAG, "VGM TASK: enable failed: %s(0x%x), handle=%p",
                                         esp_err_to_name(en_ret), en_ret, (void *)i2s_handle);
                            }
                            enable_fail_count++;
                            vTaskDelay(pdMS_TO_TICKS(10));
                            continue;  // Skip write until channel is enabled
                        }
                    }
                    // Write to I2S in DMA-sized chunks (render buffer lives in PSRAM)
                    uint32_t remaining = rendered;
                    uint32_t offset = 0;
                    while (remaining > 0) {
                        uint32_t chunk = (remaining > VGM_DMA_CHUNK_SAMPLES) ? VGM_DMA_CHUNK_SAMPLES : remaining;
                        int64_t copy_start_us = esp_timer_get_time();
                        memcpy(vgm_dma_buffer,
                               &vgm_render_buffer[offset * 2],
                               chunk * 2 * sizeof(int16_t));
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
                            first_write_logged = false;
                            vTaskDelay(pdMS_TO_TICKS(10));
                            break;
                        }
                        remaining -= chunk;
                        offset += chunk;
                    }
                } else {
                    // Playback ended
                    vgm_playing = false;
                    first_write_logged = false;  // Reset for next playback
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
        xSemaphoreGive(playback_mutex);
    } else {
        ESP_LOGW(TAG, "Timeout waiting for playback mutex in stop()");
        vgm_backend_stop(vgm_ctx);
    }

    if (i2s_handle && audio_output_is_owner(AUDIO_OUTPUT_OWNER_VGM)) {
        // Stop DMA to prevent looping the last buffer when playback stops
        i2s_channel_disable(i2s_handle);
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
    return ESP_OK;
}

esp_err_t vgm_player_resume(void) {
    if (!vgm_playing) {
        return ESP_ERR_INVALID_STATE;
    }
    vgm_paused = false;
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

TaskHandle_t vgm_player_get_task_handle(void) {
    return vgm_task_handle;
}

esp_err_t vgm_player_get_tags(vgm_tags_t *tags) {
    if (vgm_ctx == NULL || !vgm_loaded || tags == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return vgm_backend_get_tags(vgm_ctx, tags);
}

esp_err_t vgm_player_get_info(vgm_playback_info_t *info) {
    if (vgm_ctx == NULL || info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return vgm_backend_get_info(vgm_ctx, info);
}

esp_err_t vgm_player_get_chip_info(uint8_t index, vgm_chip_info_t *chip_info) {
    if (vgm_ctx == NULL || !vgm_loaded || chip_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return vgm_backend_get_chip_info(vgm_ctx, index, chip_info);
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
