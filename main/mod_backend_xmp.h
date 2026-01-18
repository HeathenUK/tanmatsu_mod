#pragma once

/**
 * @file mod_backend_xmp.h
 * @brief Compatibility wrapper for libxmp - provides unified API
 * 
 * This header defines a compatibility layer that wraps libxmp API
 * to match the unified backend interface.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct xmp_backend xmp_backend_t;

// Compatibility constants matching libxmp
#define XMP_BACKEND_MAX_CHANNELS 64  // Match XMP_MAX_CHANNELS

// Format flags (matching libxmp)
#define XMP_BACKEND_FORMAT_MONO   4  // XMP_FORMAT_MONO
#define XMP_BACKEND_FORMAT_STEREO 0

// Compatibility structures matching libxmp's xmp_frame_info
typedef struct {
    int pos;           // Order/sequence position
    int pattern;       // Current pattern index
    int row;           // Current row within pattern
    int speed;         // Speed (ticks per row)
    int bpm;           // Beats per minute
    float frame_time;  // Frame time in microseconds
    int num_channels;  // Number of channels
    struct {
        uint8_t note;   // Note (1-96, 0 = no note)
        uint8_t ins;   // Instrument (1-based, 0 = no instrument)
        uint8_t fxt;   // Effect type (0-255)
        uint8_t fxp;   // Effect parameter (0-255)
    } event;
    uint8_t volume;    // Channel volume (0-64 for MOD/XM compatibility)
    uint16_t period;   // Note period (for pitch display)
} xmp_backend_channel_info_t;

typedef struct {
    int pos;           // Order/sequence position
    int pattern;       // Current pattern index
    int row;           // Current row within pattern
    int speed;         // Speed (ticks per row)
    int bpm;           // Beats per minute
    float frame_time;  // Frame time in microseconds
    int num_channels;  // Number of channels
    xmp_backend_channel_info_t channel_info[XMP_BACKEND_MAX_CHANNELS];
} xmp_backend_frame_info_t;

// Compatibility structure matching libxmp's xmp_module_info
typedef struct {
    const char *name;  // Module name
    int chn;           // Number of channels
    int pat;           // Number of patterns
} xmp_backend_module_info_t;

/**
 * @brief Create a new libxmp backend context
 * 
 * @return Pointer to backend context, or NULL on failure
 */
xmp_backend_t* xmp_backend_create(void);

/**
 * @brief Free a backend context
 * 
 * @param ctx Backend context (can be NULL)
 */
void xmp_backend_free(xmp_backend_t *ctx);

/**
 * @brief Load a module from memory
 * 
 * @param ctx Backend context
 * @param mod_data Pointer to module file data
 * @param mod_size Size of module data in bytes
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_load_module(xmp_backend_t *ctx, const uint8_t *mod_data, size_t mod_size);

/**
 * @brief Release/unload the current module
 * 
 * @param ctx Backend context
 */
void xmp_backend_release_module(xmp_backend_t *ctx);

/**
 * @brief Start playback
 * 
 * @param ctx Backend context
 * @param sample_rate Sample rate in Hz (e.g., 44100)
 * @param format Format flag (XMP_BACKEND_FORMAT_MONO or XMP_BACKEND_FORMAT_STEREO)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_start_player(xmp_backend_t *ctx, uint32_t sample_rate, int format);

/**
 * @brief Stop playback
 * 
 * @param ctx Backend context
 */
void xmp_backend_end_player(xmp_backend_t *ctx);

/**
 * @brief Render audio buffer
 * 
 * @param ctx Backend context
 * @param buffer Output buffer (int16_t samples)
 * @param buffer_size Size of buffer in bytes
 * @param loop 0=infinite loop, 1=play once, N>1=play N times
 * @return 0 on success (more data), non-zero on end/error
 */
int xmp_backend_play_buffer(xmp_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop);

/**
 * @brief Get current frame information
 * 
 * @param ctx Backend context
 * @param frame_info Output structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_get_frame_info(xmp_backend_t *ctx, xmp_backend_frame_info_t *frame_info);

/**
 * @brief Get module information
 * 
 * @param ctx Backend context
 * @param mod_info Output structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_get_module_info(xmp_backend_t *ctx, xmp_backend_module_info_t *mod_info);

/**
 * @brief Set player parameter
 * 
 * @param ctx Backend context
 * @param param Parameter type (XMP_BACKEND_PLAYER_*)
 * @param value Parameter value
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_set_player(xmp_backend_t *ctx, int param, int value);

/**
 * @brief Control channel mute state
 * 
 * @param ctx Backend context
 * @param channel Channel number (0-based)
 * @param status -1 = query, 0 = unmute, 1 = mute, 2 = toggle
 * @return Current mute state (0 = unmuted, 1 = muted), or -1 on error
 */
int xmp_backend_channel_mute(xmp_backend_t *ctx, int channel, int status);

/**
 * @brief Get pattern row data for a specific channel
 * 
 * Used for UI display - extracts note/instrument/effects from pattern
 * 
 * @param ctx Backend context
 * @param pattern Pattern index
 * @param row Row index within pattern
 * @param channel Channel number (0-based)
 * @param note Output: Note value (0 = no note, 1-96 = note)
 * @param ins Output: Instrument number (0 = no instrument, 1+ = instrument)
 * @param fxt Output: Effect type (0-255)
 * @param fxp Output: Effect parameter (0-255)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_get_pattern_row_channel(xmp_backend_t *ctx, 
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
esp_err_t xmp_backend_get_pattern_num_rows(xmp_backend_t *ctx, int pattern, int *num_rows);

/**
 * @brief Get pattern number for a given order position
 * 
 * @param ctx Backend context
 * @param order Order position (0-based)
 * @param pattern Output: Pattern number at this order position
 * @return ESP_OK on success, error code on failure
 */
esp_err_t xmp_backend_get_order_pattern(xmp_backend_t *ctx, int order, int *pattern);

/**
 * @brief Check if module is loaded
 * 
 * @param ctx Backend context
 * @return true if module is loaded, false otherwise
 */
bool xmp_backend_is_loaded(xmp_backend_t *ctx);

/**
 * @brief Check if playback is active
 * 
 * @param ctx Backend context
 * @return true if playing, false otherwise
 */
bool xmp_backend_is_playing(xmp_backend_t *ctx);

// Player parameter constants (for xmp_backend_set_player)
#define XMP_BACKEND_PLAYER_AMP        0  // Amplification factor (0-3, lower = more headroom)
#define XMP_BACKEND_PLAYER_VOICES     1  // Max voices
#define XMP_BACKEND_PLAYER_INTERP     2  // Interpolation mode
#define XMP_BACKEND_PLAYER_DSP        3  // DSP filter mode

// Interpolation modes (matching libxmp XMP_INTERP_*)
#define XMP_BACKEND_INTERP_NEAREST    0  // XMP_INTERP_NEAREST
#define XMP_BACKEND_INTERP_LINEAR     1  // XMP_INTERP_LINEAR
#define XMP_BACKEND_INTERP_SPLINE     2  // XMP_INTERP_SPLINE
#define XMP_BACKEND_INTERP_CUBIC      3  // XMP_INTERP_CUBIC

// DSP filter modes (matching libxmp XMP_DSP_*)
#define XMP_BACKEND_DSP_LOWPASS       1  // XMP_DSP_LOWPASS
#define XMP_BACKEND_DSP_OFF           0
