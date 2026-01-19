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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "vgm_player";

// VGM player state
static vgm_backend_t *vgm_ctx = NULL;
static i2s_chan_handle_t i2s_handle = NULL;
static uint32_t sample_rate = VGM_NATIVE_SAMPLE_RATE;
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

// Audio buffers in internal SRAM for fast DMA access
static DRAM_ATTR int16_t stereo_buffer[VGM_BUFFER_SAMPLES * 2] = {0};

/**
 * @brief VGM playback task - renders VGM audio and writes to I2S
 */
static void vgm_playback_task(void *arg) {
    size_t bytes_written;

    while (1) {
        // Only play if VGM is loaded, playing flag is set, and not paused
        if (vgm_playing && !vgm_paused && vgm_loaded && vgm_ctx && i2s_handle) {
            // Acquire mutex before rendering to prevent race with stop()
            if (xSemaphoreTake(playback_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint32_t rendered = 0;

                // Re-check state after acquiring mutex
                if (vgm_playing && !vgm_paused) {
                    // Render VGM audio (stereo interleaved 16-bit)
                    rendered = vgm_backend_render(vgm_ctx, stereo_buffer, VGM_BUFFER_SAMPLES);
                }
                xSemaphoreGive(playback_mutex);

                if (rendered > 0) {
                    // Apply -6dB attenuation for headroom (same as MOD player)
                    for (int i = 0; i < (int)(rendered * 2); i++) {
                        stereo_buffer[i] = stereo_buffer[i] >> 1;
                    }

                    // Write to I2S (blocking for continuous audio)
                    i2s_channel_write(i2s_handle, stereo_buffer,
                                      rendered * 2 * sizeof(int16_t),
                                      &bytes_written, portMAX_DELAY);
                } else {
                    // Playback ended
                    vgm_playing = false;
                }
            }
        } else {
            // Not playing - write silence to prevent glitches
            if (i2s_handle) {
                memset(stereo_buffer, 0, sizeof(stereo_buffer));
                i2s_channel_write(i2s_handle, stereo_buffer, sizeof(stereo_buffer),
                                  &bytes_written, 0);  // Non-blocking
            }
            vTaskDelay(pdMS_TO_TICKS(10));
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
