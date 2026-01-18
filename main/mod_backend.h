#pragma once

/**
 * @file mod_backend.h
 * @brief Unified MOD playback backend interface
 *
 * This header provides a compile-time selectable backend for MOD playback.
 * Backend is selected via CONFIG_MOD_BACKEND in Kconfig or MOD_BACKEND_* define.
 *
 * Supported backends:
 * - libxmp: Lightweight, native C library
 * - libopenmpt: High-quality C++ library with better audio quality
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Backend selection is done via Kconfig (menuconfig)
// The CONFIG_MOD_BACKEND_* values are automatically defined by ESP-IDF
// Map Kconfig defines to our internal MOD_BACKEND_* defines
#if defined(CONFIG_MOD_BACKEND_XMP) || defined(MOD_BACKEND_XMP)
#define MOD_BACKEND_XMP 1
#elif defined(CONFIG_MOD_BACKEND_OPENMPT) || defined(MOD_BACKEND_OPENMPT)
#define MOD_BACKEND_OPENMPT 1
#elif defined(CONFIG_MOD_BACKEND_MICROMOD) || defined(MOD_BACKEND_MICROMOD)
#define MOD_BACKEND_MICROMOD 1
#else
// Fallback: default to xmp if nothing is defined (smallest, fastest)
#define MOD_BACKEND_XMP 1
#endif

// Include backend-agnostic configuration options
#include "mod_backend_config.h"

// Include the selected backend header
#if defined(MOD_BACKEND_XMP)
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

#elif defined(MOD_BACKEND_OPENMPT)
#include "mod_backend_openmpt.h"
// Use openmpt backend types
typedef openmpt_backend_t mod_backend_t;
typedef openmpt_frame_info_t mod_frame_info_t;
typedef openmpt_module_info_t mod_module_info_t;
#define MOD_FORMAT_MONO   OPENMPT_FORMAT_MONO
#define MOD_FORMAT_STEREO OPENMPT_FORMAT_STEREO
#define MOD_MAX_CHANNELS  OPENMPT_MAX_CHANNELS
#define MOD_PLAYER_VOICES OPENMPT_PLAYER_VOICES
#define MOD_PLAYER_INTERP OPENMPT_PLAYER_INTERP
#define MOD_PLAYER_DSP    OPENMPT_PLAYER_DSP
#define MOD_INTERP_NEAREST OPENMPT_INTERP_NEAREST
#define MOD_INTERP_LINEAR  OPENMPT_INTERP_LINEAR
#define MOD_INTERP_CUBIC   OPENMPT_INTERP_CUBIC
#define MOD_INTERP_SINC    OPENMPT_INTERP_SINC
#define MOD_DSP_OFF        OPENMPT_DSP_OFF
#define MOD_DSP_LOWPASS    OPENMPT_DSP_LOWPASS

// Map backend functions to unified interface
#define mod_backend_create                openmpt_backend_create
#define mod_backend_free                  openmpt_backend_free
#define mod_backend_load_module           openmpt_backend_load_module
#define mod_backend_release_module        openmpt_backend_release_module
#define mod_backend_start_player          openmpt_backend_start_player
#define mod_backend_end_player            openmpt_backend_end_player
#define mod_backend_play_buffer           openmpt_backend_play_buffer
#define mod_backend_get_frame_info        openmpt_backend_get_frame_info
#define mod_backend_get_module_info       openmpt_backend_get_module_info
#define mod_backend_set_player            openmpt_backend_set_player
#define mod_backend_channel_mute          openmpt_backend_channel_mute
#define mod_backend_get_pattern_row_channel openmpt_backend_get_pattern_row_channel
#define mod_backend_get_pattern_num_rows   openmpt_backend_get_pattern_num_rows
#define mod_backend_get_order_pattern      openmpt_backend_get_order_pattern
#define mod_backend_is_loaded             openmpt_backend_is_loaded
#define mod_backend_is_playing            openmpt_backend_is_playing

#elif defined(MOD_BACKEND_MICROMOD)
#include "mod_backend_micromod.h"
// Use micromod (IBXM) backend types
typedef micromod_backend_t mod_backend_t;
typedef micromod_frame_info_t mod_frame_info_t;
typedef micromod_module_info_t mod_module_info_t;
#define MOD_FORMAT_MONO   MICROMOD_FORMAT_MONO
#define MOD_FORMAT_STEREO MICROMOD_FORMAT_STEREO
#define MOD_MAX_CHANNELS  MICROMOD_MAX_CHANNELS
#define MOD_PLAYER_VOICES MICROMOD_PLAYER_VOICES
#define MOD_PLAYER_INTERP MICROMOD_PLAYER_INTERP
#define MOD_PLAYER_DSP    MICROMOD_PLAYER_DSP
#define MOD_INTERP_NEAREST MICROMOD_INTERP_NEAREST
#define MOD_INTERP_LINEAR  MICROMOD_INTERP_LINEAR
#define MOD_INTERP_CUBIC   MICROMOD_INTERP_CUBIC
#define MOD_INTERP_SINC    MICROMOD_INTERP_SINC
#define MOD_DSP_OFF        MICROMOD_DSP_OFF
#define MOD_DSP_LOWPASS    MICROMOD_DSP_LOWPASS

// Map backend functions to unified interface
#define mod_backend_create                micromod_backend_create
#define mod_backend_free                  micromod_backend_free
#define mod_backend_load_module           micromod_backend_load_module
#define mod_backend_release_module        micromod_backend_release_module
#define mod_backend_start_player          micromod_backend_start_player
#define mod_backend_end_player            micromod_backend_end_player
#define mod_backend_play_buffer           micromod_backend_play_buffer
#define mod_backend_get_frame_info        micromod_backend_get_frame_info
#define mod_backend_get_module_info       micromod_backend_get_module_info
#define mod_backend_set_player            micromod_backend_set_player
#define mod_backend_channel_mute          micromod_backend_channel_mute
#define mod_backend_get_pattern_row_channel micromod_backend_get_pattern_row_channel
#define mod_backend_get_pattern_num_rows   micromod_backend_get_pattern_num_rows
#define mod_backend_get_order_pattern      micromod_backend_get_order_pattern
#define mod_backend_is_loaded             micromod_backend_is_loaded
#define mod_backend_is_playing            micromod_backend_is_playing

#else
#error "No MOD backend selected! Define MOD_BACKEND_XMP, MOD_BACKEND_OPENMPT, or MOD_BACKEND_MICROMOD"
#endif

// Unified backend name function (must be implemented by each backend)
static inline const char* mod_backend_get_name(void) {
#if defined(MOD_BACKEND_XMP)
    return "libxmp";
#elif defined(MOD_BACKEND_OPENMPT)
    return "libopenmpt";
#elif defined(MOD_BACKEND_MICROMOD)
    return "micromod";
#else
    return "unknown";
#endif
}
