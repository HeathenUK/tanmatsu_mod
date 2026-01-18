#pragma once

/**
 * @file mod_backend_openmpt.h
 * @brief Compatibility wrapper for libopenmpt - provides libxmp-like API
 * 
 * This header defines a compatibility layer that maps libxmp API calls
 * to libopenmpt equivalents, allowing minimal changes to existing code.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct openmpt_backend openmpt_backend_t;

// Compatibility constants matching libxmp
#define OPENMPT_MAX_CHANNELS 64  // Match XMP_MAX_CHANNELS or libopenmpt's limit

// Format flags (matching libxmp)
#define OPENMPT_FORMAT_MONO   4
#define OPENMPT_FORMAT_STEREO 0

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
} openmpt_channel_info_t;

typedef struct {
    int pos;           // Order/sequence position
    int pattern;       // Current pattern index
    int row;           // Current row within pattern
    int speed;         // Speed (ticks per row)
    int bpm;           // Beats per minute
    float frame_time;  // Frame time in microseconds
    int num_channels;  // Number of channels
    openmpt_channel_info_t channel_info[OPENMPT_MAX_CHANNELS];
} openmpt_frame_info_t;

// Compatibility structure matching libxmp's xmp_module_info
// libxmp uses xmp_module_info which contains a pointer to xmp_module
// We create a compatibility structure that matches this
typedef struct {
    const char *name;  // Module name
    int chn;           // Number of channels
    int pat;           // Number of patterns
    // Pattern access will be via functions, not direct struct access
} openmpt_module_info_t;

// Compatibility structure for xmp_module (used by xmp_module_info.mod)
typedef struct {
    char name[64];    // Module name (fixed size for compatibility)
    int chn;           // Number of channels
    int pat;           // Number of patterns
} openmpt_module_compat_t;

// Compatibility structure matching libxmp's xmp_module_info exactly
// This is what the UI code expects
typedef struct {
    struct {
        char name[64];    // Module name
        int chn;          // Number of channels
        int pat;          // Number of patterns
    } *mod;               // Pointer to module data (for compatibility with libxmp)
} xmp_module_info_compat_t;

/**
 * @brief Create a new libopenmpt backend context
 * 
 * Equivalent to: xmp_create_context()
 * 
 * @return Pointer to backend context, or NULL on failure
 */
openmpt_backend_t* openmpt_backend_create(void);

/**
 * @brief Free a backend context
 * 
 * Equivalent to: xmp_free_context()
 * 
 * @param ctx Backend context (can be NULL)
 */
void openmpt_backend_free(openmpt_backend_t *ctx);

/**
 * @brief Load a module from memory
 * 
 * Equivalent to: xmp_load_module_from_memory()
 * 
 * @param ctx Backend context
 * @param mod_data Pointer to module file data
 * @param mod_size Size of module data in bytes
 * @return ESP_OK on success, error code on failure
 */
esp_err_t openmpt_backend_load_module(openmpt_backend_t *ctx, const uint8_t *mod_data, size_t mod_size);

/**
 * @brief Release/unload the current module
 * 
 * Equivalent to: xmp_release_module()
 * 
 * @param ctx Backend context
 */
void openmpt_backend_release_module(openmpt_backend_t *ctx);

/**
 * @brief Start playback
 * 
 * Equivalent to: xmp_start_player()
 * 
 * @param ctx Backend context
 * @param sample_rate Sample rate in Hz (e.g., 44100)
 * @param format Format flag (OPENMPT_FORMAT_MONO or OPENMPT_FORMAT_STEREO)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t openmpt_backend_start_player(openmpt_backend_t *ctx, uint32_t sample_rate, int format);

/**
 * @brief Stop playback
 * 
 * Equivalent to: xmp_end_player()
 * 
 * @param ctx Backend context
 */
void openmpt_backend_end_player(openmpt_backend_t *ctx);

/**
 * @brief Render audio buffer
 * 
 * Equivalent to: xmp_play_buffer()
 * 
 * @param ctx Backend context
 * @param buffer Output buffer (int16_t samples)
 * @param buffer_size Size of buffer in bytes
 * @param loop 0=infinite loop, 1=play once, N>1=play N times
 * @return 0 on success (more data), non-zero on end/error
 */
int openmpt_backend_play_buffer(openmpt_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop);

/**
 * @brief Get current frame information
 * 
 * Equivalent to: xmp_get_frame_info()
 * 
 * @param ctx Backend context
 * @param frame_info Output structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t openmpt_backend_get_frame_info(openmpt_backend_t *ctx, openmpt_frame_info_t *frame_info);

/**
 * @brief Get module information
 * 
 * Equivalent to: xmp_get_module_info()
 * 
 * @param ctx Backend context
 * @param mod_info Output structure to fill
 * @return ESP_OK on success, error code on failure
 */
esp_err_t openmpt_backend_get_module_info(openmpt_backend_t *ctx, openmpt_module_info_t *mod_info);

/**
 * @brief Set player parameter
 * 
 * Equivalent to: xmp_set_player()
 * 
 * @param ctx Backend context
 * @param param Parameter type (implementation-specific)
 * @param value Parameter value
 * @return ESP_OK on success, error code on failure
 */
esp_err_t openmpt_backend_set_player(openmpt_backend_t *ctx, int param, int value);

/**
 * @brief Control channel mute state
 * 
 * Equivalent to: xmp_channel_mute()
 * 
 * @param ctx Backend context
 * @param channel Channel number (0-based)
 * @param status -1 = query, 0 = unmute, 1 = mute, 2 = toggle
 * @return Current mute state (0 = unmuted, 1 = muted), or error code
 */
int openmpt_backend_channel_mute(openmpt_backend_t *ctx, int channel, int status);

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
esp_err_t openmpt_backend_get_pattern_row_channel(openmpt_backend_t *ctx, 
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
esp_err_t openmpt_backend_get_pattern_num_rows(openmpt_backend_t *ctx, int pattern, int *num_rows);

/**
 * @brief Get pattern number for a given order position
 * 
 * @param ctx Backend context
 * @param order Order position (0-based)
 * @param pattern Output: Pattern number at this order position
 * @return ESP_OK on success, error code on failure
 */
esp_err_t openmpt_backend_get_order_pattern(openmpt_backend_t *ctx, int order, int *pattern);

/**
 * @brief Check if module is loaded
 * 
 * @param ctx Backend context
 * @return true if module is loaded, false otherwise
 */
bool openmpt_backend_is_loaded(openmpt_backend_t *ctx);

/**
 * @brief Check if playback is active
 * 
 * @param ctx Backend context
 * @return true if playing, false otherwise
 */
bool openmpt_backend_is_playing(openmpt_backend_t *ctx);

// Player parameter constants (for openmpt_backend_set_player)
// These map to libopenmpt's render parameters
#define OPENMPT_PLAYER_VOICES     1  // Max voices (not directly supported, use render param)
#define OPENMPT_PLAYER_INTERP    2  // Interpolation mode
#define OPENMPT_PLAYER_DSP       3  // DSP filter mode

// Interpolation modes
#define OPENMPT_INTERP_NEAREST   0
#define OPENMPT_INTERP_LINEAR    1
#define OPENMPT_INTERP_CUBIC     2
#define OPENMPT_INTERP_SINC      3  // Highest quality

// DSP filter modes
#define OPENMPT_DSP_LOWPASS      1
#define OPENMPT_DSP_OFF          0
