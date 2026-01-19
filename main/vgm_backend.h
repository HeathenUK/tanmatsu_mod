#pragma once

/**
 * @file vgm_backend.h
 * @brief VGM/VGZ playback backend using libvgm
 *
 * This module provides a C interface to libvgm for playing VGM (Video Game Music)
 * files. VGM files are chip register logs from classic game consoles like
 * Sega Genesis, Master System, etc.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// VGM native sample rate
#define VGM_NATIVE_SAMPLE_RATE 44100

// Maximum tag string length
#define VGM_TAG_MAX_LEN 256

/**
 * @brief VGM metadata (GD3 tags)
 */
typedef struct {
    char title[VGM_TAG_MAX_LEN];
    char title_jpn[VGM_TAG_MAX_LEN];
    char game[VGM_TAG_MAX_LEN];
    char game_jpn[VGM_TAG_MAX_LEN];
    char system[VGM_TAG_MAX_LEN];
    char system_jpn[VGM_TAG_MAX_LEN];
    char author[VGM_TAG_MAX_LEN];
    char author_jpn[VGM_TAG_MAX_LEN];
    char date[64];
    char encoded_by[VGM_TAG_MAX_LEN];
    char comment[VGM_TAG_MAX_LEN];
} vgm_tags_t;

/**
 * @brief VGM playback information
 */
typedef struct {
    uint32_t total_samples;      // Total samples in file
    uint32_t loop_samples;       // Samples in loop section (0 if no loop)
    uint32_t current_sample;     // Current playback position in samples
    double total_time_sec;       // Total time in seconds
    double current_time_sec;     // Current time in seconds
    double loop_time_sec;        // Loop point time in seconds (0 if no loop)
    uint32_t sample_rate;        // Output sample rate
    uint8_t num_chips;           // Number of sound chips used
    bool has_loop;               // Whether file has loop point
    bool is_playing;             // Whether playback is active
    bool has_ended;              // Whether playback reached end
} vgm_playback_info_t;

/**
 * @brief Chip type information
 */
typedef struct {
    uint32_t type;               // Chip type ID
    const char *name;            // Chip name (e.g., "YM2612", "SN76489")
    uint32_t clock;              // Chip clock rate
} vgm_chip_info_t;

/**
 * @brief Opaque VGM backend context
 */
typedef struct vgm_backend vgm_backend_t;

/**
 * @brief Create VGM backend context
 *
 * @return Pointer to backend context, or NULL on failure
 */
vgm_backend_t *vgm_backend_create(void);

/**
 * @brief Free VGM backend context
 *
 * @param ctx Backend context
 */
void vgm_backend_free(vgm_backend_t *ctx);

/**
 * @brief Load VGM/VGZ file from memory
 *
 * Automatically detects and decompresses VGZ (gzip-compressed VGM) files.
 *
 * @param ctx Backend context
 * @param data Pointer to file data
 * @param size Size of file data in bytes
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation failure,
 *         ESP_ERR_INVALID_ARG on invalid file
 */
esp_err_t vgm_backend_load(vgm_backend_t *ctx, const uint8_t *data, size_t size);

/**
 * @brief Unload current VGM file
 *
 * @param ctx Backend context
 * @return ESP_OK on success
 */
esp_err_t vgm_backend_unload(vgm_backend_t *ctx);

/**
 * @brief Start playback
 *
 * @param ctx Backend context
 * @param sample_rate Output sample rate (typically 44100 or 48000)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no file loaded
 */
esp_err_t vgm_backend_start(vgm_backend_t *ctx, uint32_t sample_rate);

/**
 * @brief Stop playback
 *
 * @param ctx Backend context
 * @return ESP_OK on success
 */
esp_err_t vgm_backend_stop(vgm_backend_t *ctx);

/**
 * @brief Render audio samples
 *
 * Renders interleaved stereo 16-bit samples.
 *
 * @param ctx Backend context
 * @param buffer Output buffer for samples (stereo interleaved)
 * @param num_samples Number of sample frames to render
 * @return Number of samples actually rendered, or 0 on error/end
 */
uint32_t vgm_backend_render(vgm_backend_t *ctx, int16_t *buffer, uint32_t num_samples);

/**
 * @brief Get GD3 tags (metadata)
 *
 * @param ctx Backend context
 * @param tags Pointer to tags structure to fill
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no file loaded
 */
esp_err_t vgm_backend_get_tags(vgm_backend_t *ctx, vgm_tags_t *tags);

/**
 * @brief Get playback information
 *
 * @param ctx Backend context
 * @param info Pointer to info structure to fill
 * @return ESP_OK on success
 */
esp_err_t vgm_backend_get_info(vgm_backend_t *ctx, vgm_playback_info_t *info);

/**
 * @brief Get chip information
 *
 * @param ctx Backend context
 * @param index Chip index (0 to num_chips-1)
 * @param chip_info Pointer to chip info structure to fill
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range
 */
esp_err_t vgm_backend_get_chip_info(vgm_backend_t *ctx, uint8_t index, vgm_chip_info_t *chip_info);

/**
 * @brief Check if backend is loaded
 *
 * @param ctx Backend context
 * @return true if a file is loaded
 */
bool vgm_backend_is_loaded(vgm_backend_t *ctx);

/**
 * @brief Check if playback is active
 *
 * @param ctx Backend context
 * @return true if playing
 */
bool vgm_backend_is_playing(vgm_backend_t *ctx);

/**
 * @brief Seek to position
 *
 * @param ctx Backend context
 * @param sample_pos Position in samples
 * @return ESP_OK on success
 */
esp_err_t vgm_backend_seek(vgm_backend_t *ctx, uint32_t sample_pos);

/**
 * @brief Set number of loops before stopping
 *
 * @param ctx Backend context
 * @param loops Number of loops (0 = infinite, 1 = play once, 2 = loop once, etc.)
 */
void vgm_backend_set_loop_count(vgm_backend_t *ctx, uint32_t loops);

/**
 * @brief Set fade out duration
 *
 * @param ctx Backend context
 * @param fade_ms Fade out duration in milliseconds
 */
void vgm_backend_set_fade_time(vgm_backend_t *ctx, uint32_t fade_ms);

#ifdef __cplusplus
}
#endif
