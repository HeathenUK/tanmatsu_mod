#pragma once

/**
 * @file mod_backend_micromod.h
 * @brief Compatibility wrapper for micromod (IBXM) - provides unified API
 *
 * This header defines a compatibility layer that wraps IBXM API
 * to match the unified backend interface.
 *
 * IBXM supports MOD, XM, and S3M formats with pattern data access.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct micromod_backend micromod_backend_t;

// Compatibility constants
#define MICROMOD_MAX_CHANNELS 64

// Format flags (matching other backends)
#define MICROMOD_FORMAT_MONO   4
#define MICROMOD_FORMAT_STEREO 0

// Compatibility structures matching the unified backend interface
typedef struct {
    int pos;           // Order/sequence position
    int pattern;       // Current pattern index
    int row;           // Current row within pattern
    int speed;         // Speed (ticks per row)
    int bpm;           // Beats per minute (tempo)
    float frame_time;  // Frame time in microseconds
    int num_channels;  // Number of channels
    struct {
        uint8_t note;   // Note (0 = no note, 1-96 = note, 97 = note off)
        uint8_t ins;    // Instrument (0 = no instrument, 1+ = instrument)
        uint8_t fxt;    // Effect type
        uint8_t fxp;    // Effect parameter
    } event;
    uint8_t volume;    // Channel volume (0-64 for MOD/XM compatibility)
    uint16_t period;   // Note period (for pitch display)
} micromod_channel_info_t;

typedef struct {
    int pos;           // Order/sequence position
    int pattern;       // Current pattern index
    int row;           // Current row within pattern
    int speed;         // Speed (ticks per row)
    int bpm;           // Beats per minute (tempo)
    float frame_time;  // Frame time in microseconds
    int num_channels;  // Number of channels
    micromod_channel_info_t channel_info[MICROMOD_MAX_CHANNELS];
} micromod_frame_info_t;

// Module info structure
typedef struct {
    const char *name;  // Module name
    int chn;           // Number of channels
    int pat;           // Number of patterns
} micromod_module_info_t;

/**
 * @brief Create a new micromod backend context
 *
 * @return Pointer to backend context, or NULL on failure
 */
micromod_backend_t* micromod_backend_create(void);

/**
 * @brief Free a backend context
 *
 * @param ctx Backend context (can be NULL)
 */
void micromod_backend_free(micromod_backend_t *ctx);

/**
 * @brief Load a module from memory
 *
 * @param ctx Backend context
 * @param mod_data Pointer to module file data
 * @param mod_size Size of module data in bytes
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_load_module(micromod_backend_t *ctx, const uint8_t *mod_data, size_t mod_size);

/**
 * @brief Release/unload the current module
 *
 * @param ctx Backend context
 */
void micromod_backend_release_module(micromod_backend_t *ctx);

/**
 * @brief Start playback
 *
 * @param ctx Backend context
 * @param sample_rate Sample rate in Hz (e.g., 44100)
 * @param format Format flag (MICROMOD_FORMAT_MONO or MICROMOD_FORMAT_STEREO)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_start_player(micromod_backend_t *ctx, uint32_t sample_rate, int format);

/**
 * @brief Stop playback
 *
 * @param ctx Backend context
 */
void micromod_backend_end_player(micromod_backend_t *ctx);

/**
 * @brief Render audio buffer
 *
 * @param ctx Backend context
 * @param buffer Output buffer (int16_t samples)
 * @param buffer_size Size of buffer in bytes
 * @param loop 0=infinite loop, 1=play once, N>1=play N times
 * @return 0 on success (more data), non-zero on end/error
 */
int micromod_backend_play_buffer(micromod_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop);

/**
 * @brief Get current frame information
 *
 * @param ctx Backend context
 * @param frame_info Output structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_get_frame_info(micromod_backend_t *ctx, micromod_frame_info_t *frame_info);

/**
 * @brief Get module information
 *
 * @param ctx Backend context
 * @param mod_info Output structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_get_module_info(micromod_backend_t *ctx, micromod_module_info_t *mod_info);

/**
 * @brief Set player parameter
 *
 * @param ctx Backend context
 * @param param Parameter type (MICROMOD_PLAYER_*)
 * @param value Parameter value
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_set_player(micromod_backend_t *ctx, int param, int value);

/**
 * @brief Control channel mute state
 *
 * @param ctx Backend context
 * @param channel Channel number (0-based)
 * @param status -1 = query, 0 = unmute, 1 = mute, 2 = toggle
 * @return Current mute state (0 = unmuted, 1 = muted), or -1 on error
 */
int micromod_backend_channel_mute(micromod_backend_t *ctx, int channel, int status);

/**
 * @brief Get pattern row data for a specific channel
 *
 * @param ctx Backend context
 * @param pattern Pattern index
 * @param row Row index within pattern
 * @param channel Channel number (0-based)
 * @param note Output: Note value
 * @param ins Output: Instrument number
 * @param fxt Output: Effect type
 * @param fxp Output: Effect parameter
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_get_pattern_row_channel(micromod_backend_t *ctx,
                                                    int pattern, int row, int channel,
                                                    uint8_t *note, uint8_t *ins,
                                                    uint8_t *fxt, uint8_t *fxp);

/**
 * @brief Get number of rows in a pattern
 *
 * @param ctx Backend context
 * @param pattern Pattern index
 * @param num_rows Output: Number of rows in the pattern
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_get_pattern_num_rows(micromod_backend_t *ctx, int pattern, int *num_rows);

/**
 * @brief Get pattern number for a given order position
 *
 * @param ctx Backend context
 * @param order Order position (0-based)
 * @param pattern Output: Pattern number at this order position
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micromod_backend_get_order_pattern(micromod_backend_t *ctx, int order, int *pattern);

/**
 * @brief Check if module is loaded
 *
 * @param ctx Backend context
 * @return true if module is loaded, false otherwise
 */
bool micromod_backend_is_loaded(micromod_backend_t *ctx);

/**
 * @brief Check if playback is active
 *
 * @param ctx Backend context
 * @return true if playing, false otherwise
 */
bool micromod_backend_is_playing(micromod_backend_t *ctx);

// Player parameter constants
#define MICROMOD_PLAYER_VOICES     1  // Max voices (not applicable)
#define MICROMOD_PLAYER_INTERP     2  // Interpolation mode
#define MICROMOD_PLAYER_DSP        3  // DSP filter mode (not applicable)

// Interpolation modes (IBXM supports 0=none, 1=linear, 2=sinc)
#define MICROMOD_INTERP_NEAREST    0
#define MICROMOD_INTERP_LINEAR     1
#define MICROMOD_INTERP_CUBIC      1  // Map to linear (not supported)
#define MICROMOD_INTERP_SINC       2

// DSP filter modes (not supported by IBXM)
#define MICROMOD_DSP_OFF           0
#define MICROMOD_DSP_LOWPASS       0
