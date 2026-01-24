#pragma once

/**
 * @file mod_backend.h
 * @brief Unified MOD playback backend interface
 *
 * This header provides a libxmp-only backend for MOD playback.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// libxmp backend only
#define MOD_BACKEND_XMP 1

// Include backend-agnostic configuration options
#include "mod_backend_config.h"

// Include the backend header
#include "mod_backend_xmp.h"
// Use xmp backend types
typedef xmp_backend_t mod_backend_t;
typedef xmp_backend_frame_info_t mod_frame_info_t;
typedef xmp_backend_module_info_t mod_module_info_t;
#define MOD_FORMAT_MONO   XMP_BACKEND_FORMAT_MONO
#define MOD_FORMAT_STEREO XMP_BACKEND_FORMAT_STEREO
#define MOD_MAX_CHANNELS  XMP_BACKEND_MAX_CHANNELS
#define MOD_PLAYER_VOICES XMP_BACKEND_PLAYER_VOICES
#define MOD_PLAYER_INTERP XMP_BACKEND_PLAYER_INTERP
#define MOD_PLAYER_DSP    XMP_BACKEND_PLAYER_DSP
#define MOD_INTERP_NEAREST XMP_BACKEND_INTERP_NEAREST
#define MOD_INTERP_LINEAR  XMP_BACKEND_INTERP_LINEAR
#define MOD_INTERP_CUBIC   XMP_BACKEND_INTERP_CUBIC
#define MOD_INTERP_SINC    XMP_BACKEND_INTERP_SPLINE  // Use spline as closest match
#define MOD_DSP_OFF        XMP_BACKEND_DSP_OFF
#define MOD_DSP_LOWPASS    XMP_BACKEND_DSP_LOWPASS

// Map backend functions to unified interface
#define mod_backend_create                xmp_backend_create
#define mod_backend_free                  xmp_backend_free
#define mod_backend_load_module           xmp_backend_load_module
#define mod_backend_release_module        xmp_backend_release_module
#define mod_backend_start_player          xmp_backend_start_player
#define mod_backend_end_player            xmp_backend_end_player
#define mod_backend_play_buffer           xmp_backend_play_buffer
#define mod_backend_get_frame_info        xmp_backend_get_frame_info
#define mod_backend_get_module_info       xmp_backend_get_module_info
#define mod_backend_set_player            xmp_backend_set_player
#define mod_backend_channel_mute          xmp_backend_channel_mute
#define mod_backend_get_pattern_row_channel xmp_backend_get_pattern_row_channel
#define mod_backend_get_pattern_num_rows   xmp_backend_get_pattern_num_rows
#define mod_backend_get_order_pattern      xmp_backend_get_order_pattern
#define mod_backend_is_loaded             xmp_backend_is_loaded
#define mod_backend_is_playing            xmp_backend_is_playing

// Unified backend name function (must be implemented by each backend)
static inline const char* mod_backend_get_name(void) {
    return "libxmp";
}
