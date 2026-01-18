#pragma once

/**
 * @file mod_backend_config.h
 * @brief MOD backend configuration
 * 
 * Configure MOD playback settings. All settings apply to both backends (libxmp and libopenmpt).
 */

// ============================================================================
// BACKEND SELECTION
// ============================================================================
// NOTE: Backend selection is now done via menuconfig (Kconfig)
// Run 'idf.py menuconfig' and navigate to "MOD Playback Configuration"
// Or set CONFIG_MOD_BACKEND_XMP=y or CONFIG_MOD_BACKEND_OPENMPT=y in sdkconfig
//
// The old #define method is still supported for backward compatibility:
// #define MOD_BACKEND_XMP
// #define MOD_BACKEND_OPENMPT

// ============================================================================
// AUDIO CONFIGURATION
// ============================================================================

/**
 * @brief Sample rate for MOD playback
 * 
 * Common values: 22050, 44100, 48000
 * Lower rates use less CPU but reduce audio quality.
 * Higher rates improve quality but increase CPU usage.
 * 
 * Default: 22050 Hz (half CD quality, lower CPU usage)
 */
#ifndef MOD_CONFIG_SAMPLE_RATE
#define MOD_CONFIG_SAMPLE_RATE 48000
#endif

/**
 * @brief Audio output format
 * 
 * - MOD_CONFIG_FORMAT_MONO: Mono output (converted to stereo for I2S)
 *   - Uses less memory and CPU (single channel rendering)
 *   - Recommended for embedded systems
 * 
 * - MOD_CONFIG_FORMAT_STEREO: Native stereo output
 *   - Better for modules with stereo panning
 *   - Uses more memory and CPU
 * 
 * Default: MOD_CONFIG_FORMAT_MONO
 */
#ifndef MOD_CONFIG_FORMAT
#define MOD_CONFIG_FORMAT MOD_CONFIG_FORMAT_MONO
#endif

#define MOD_CONFIG_FORMAT_MONO   4  // Mono (matches MOD_FORMAT_MONO)
#define MOD_CONFIG_FORMAT_STEREO 0  // Stereo (matches MOD_FORMAT_STEREO)

// ============================================================================
// BUFFER CONFIGURATION
// ============================================================================

/**
 * @brief Audio buffer size in samples
 * 
 * Larger buffers:
 * - Reduce CPU usage (fewer DMA calls)
 * - Increase latency
 * - Use more memory
 * - Better for systems with high UI load
 * 
 * Smaller buffers:
 * - Lower latency
 * - Higher CPU usage
 * - Use less memory
 * - Better for low-power systems
 * 
 * Buffer time at 44.1kHz:
 * - 2048 samples = ~46ms
 * - 4096 samples = ~93ms (default, exceeds ~51ms UI render cycle)
 * - 8192 samples = ~186ms
 * 
 * Default: 4096 samples
 */
#ifndef MOD_CONFIG_BUFFER_SAMPLES
#define MOD_CONFIG_BUFFER_SAMPLES 4096
#endif

// ============================================================================
// QUALITY SETTINGS
// ============================================================================

/**
 * @brief Interpolation mode
 * 
 * Higher quality:
 * - Better audio fidelity
 * - More CPU usage
 * - Recommended for high sample rates
 * 
 * Lower quality:
 * - Faster processing
 * - Less CPU usage
 * - Recommended for embedded/low-power systems
 * 
 * Options:
 * - MOD_CONFIG_INTERP_NEAREST: Fastest, lowest quality
 * - MOD_CONFIG_INTERP_LINEAR: Good balance (default)
 * - MOD_CONFIG_INTERP_CUBIC: Higher quality, more CPU
 * - MOD_CONFIG_INTERP_SINC: Highest quality, most CPU
 * 
 * Default: MOD_CONFIG_INTERP_LINEAR
 */
#ifndef MOD_CONFIG_INTERP
#define MOD_CONFIG_INTERP MOD_CONFIG_INTERP_LINEAR
#endif

#define MOD_CONFIG_INTERP_NEAREST 0
#define MOD_CONFIG_INTERP_LINEAR  1
#define MOD_CONFIG_INTERP_CUBIC   2
#define MOD_CONFIG_INTERP_SINC    3

/**
 * @brief Enable/disable DSP filtering
 * 
 * DSP filtering applies a lowpass filter to reduce aliasing.
 * Recommended to keep enabled for best audio quality.
 * 
 * - 0: Disabled (may have aliasing artifacts)
 * - 1: Enabled (default)
 * 
 * Default: 1 (enabled)
 */
#ifndef MOD_CONFIG_DSP_ENABLE
#define MOD_CONFIG_DSP_ENABLE 1
#endif

// ============================================================================
// PERFORMANCE SETTINGS
// ============================================================================

/**
 * @brief Task stack size in bytes
 * 
 * MOD playback runs in a separate FreeRTOS task.
 * Stack is allocated from PSRAM to avoid SRAM constraints.
 * 
 * Larger stack:
 * - Allows more complex modules
 * - Uses more PSRAM
 * 
 * Smaller stack:
 * - May cause stack overflow with complex modules
 * - Uses less PSRAM
 * 
 * Default: 16KB (16384 bytes)
 */
#ifndef MOD_CONFIG_TASK_STACK_SIZE
#define MOD_CONFIG_TASK_STACK_SIZE (16 * 1024)
#endif

// ============================================================================
// BEHAVIOR SETTINGS
// ============================================================================

/**
 * @brief Default loop behavior
 * 
 * - 0: Loop indefinitely (default)
 * - 1: Play once, stop at end
 * - N>1: Play N times, then stop
 * 
 * This can be changed at runtime, but sets the initial default.
 * 
 * Default: 0 (infinite loop)
 */
#ifndef MOD_CONFIG_DEFAULT_LOOP
#define MOD_CONFIG_DEFAULT_LOOP 0
#endif

// ============================================================================
// DEBUG/LOGGING
// ============================================================================

/**
 * @brief Enable verbose MOD player logging
 * 
 * When enabled, logs additional information about:
 * - Module loading
 * - Audio buffer statistics
 * - Playback state changes
 * 
 * - 0: Minimal logging (default)
 * - 1: Verbose logging
 * 
 * Default: 0 (disabled)
 */
#ifndef MOD_CONFIG_VERBOSE_LOG
#define MOD_CONFIG_VERBOSE_LOG 0
#endif
