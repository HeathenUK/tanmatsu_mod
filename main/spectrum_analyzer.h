#pragma once

/**
 * @file spectrum_analyzer.h
 * @brief FFT-based spectrum analyzer for real-time audio visualization
 *
 * This module provides frequency spectrum analysis using ESP-IDF's DSP library
 * with hardware acceleration on ESP32-P4. It processes audio samples to produce
 * frequency band levels suitable for visualization.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// =============================================================================
// Configuration
// =============================================================================

#define SPECTRUM_FFT_SIZE       256     // 256-point FFT (good balance of resolution/speed)
#define SPECTRUM_NUM_BANDS      16      // Number of output frequency bands
#define SPECTRUM_SAMPLE_RATE    44100   // Expected sample rate (can be overridden)

// =============================================================================
// Types
// =============================================================================

/**
 * @brief Spectrum analyzer context (opaque)
 */
typedef struct spectrum_analyzer spectrum_analyzer_t;

/**
 * @brief Spectrum analyzer configuration
 */
typedef struct {
    uint32_t sample_rate;       // Sample rate in Hz
    float decay_rate;           // Level decay rate per update (0.0-1.0, higher = faster decay)
    float peak_decay_rate;      // Peak hold decay rate (0.0-1.0, higher = faster)
    uint32_t peak_hold_frames;  // Frames to hold peak before decay
} spectrum_config_t;

/**
 * @brief Default configuration
 */
#define SPECTRUM_CONFIG_DEFAULT() { \
    .sample_rate = SPECTRUM_SAMPLE_RATE, \
    .decay_rate = 0.15f, \
    .peak_decay_rate = 0.02f, \
    .peak_hold_frames = 20 \
}

// =============================================================================
// API Functions
// =============================================================================

/**
 * @brief Initialize the spectrum analyzer
 *
 * Allocates internal buffers and initializes DSP components.
 *
 * @param config Configuration options (NULL for defaults)
 * @return Pointer to analyzer context, or NULL on failure
 */
spectrum_analyzer_t *spectrum_init(const spectrum_config_t *config);

/**
 * @brief Free spectrum analyzer resources
 *
 * @param analyzer Analyzer context (can be NULL)
 */
void spectrum_free(spectrum_analyzer_t *analyzer);

/**
 * @brief Process audio samples
 *
 * Feed audio samples into the analyzer. Call this periodically with new audio data.
 * The analyzer maintains an internal ring buffer and processes when enough
 * samples are available.
 *
 * @param analyzer Analyzer context
 * @param samples Pointer to int16_t audio samples (mono)
 * @param count Number of samples
 */
void spectrum_process_samples(spectrum_analyzer_t *analyzer,
                              const int16_t *samples, size_t count);

/**
 * @brief Get current frequency band levels
 *
 * Returns the current level for each frequency band, suitable for visualization.
 * Levels are in the range 0-255.
 *
 * @param analyzer Analyzer context
 * @param bands Output array (must have space for at least num_bands elements)
 * @param num_bands Number of bands to retrieve (max SPECTRUM_NUM_BANDS)
 */
void spectrum_get_bands(spectrum_analyzer_t *analyzer,
                        uint8_t *bands, size_t num_bands);

/**
 * @brief Get peak hold levels for each band
 *
 * Returns the peak level for each frequency band. Peaks decay slowly over time.
 *
 * @param analyzer Analyzer context
 * @param peaks Output array (must have space for at least num_bands elements)
 * @param num_bands Number of bands to retrieve
 */
void spectrum_get_peaks(spectrum_analyzer_t *analyzer,
                        uint8_t *peaks, size_t num_bands);

/**
 * @brief Get both band levels and peaks in one call
 *
 * More efficient than calling spectrum_get_bands and spectrum_get_peaks separately.
 *
 * @param analyzer Analyzer context
 * @param bands Output array for band levels (can be NULL)
 * @param peaks Output array for peak levels (can be NULL)
 * @param num_bands Number of bands to retrieve
 */
void spectrum_get_bands_and_peaks(spectrum_analyzer_t *analyzer,
                                   uint8_t *bands, uint8_t *peaks,
                                   size_t num_bands);

/**
 * @brief Reset analyzer state
 *
 * Clears all internal buffers and resets levels to zero.
 *
 * @param analyzer Analyzer context
 */
void spectrum_reset(spectrum_analyzer_t *analyzer);

/**
 * @brief Update decay (call once per frame)
 *
 * Call this at your frame rate to apply smooth decay to levels and peaks.
 * Should be called even when not processing new samples to maintain animation.
 *
 * @param analyzer Analyzer context
 */
void spectrum_update_decay(spectrum_analyzer_t *analyzer);

/**
 * @brief Set decay rates
 *
 * @param analyzer Analyzer context
 * @param level_decay Decay rate for band levels (0.0-1.0)
 * @param peak_decay Decay rate for peak indicators (0.0-1.0)
 */
void spectrum_set_decay(spectrum_analyzer_t *analyzer,
                        float level_decay, float peak_decay);

/**
 * @brief Check if spectrum analysis is supported
 *
 * @return true if hardware-accelerated FFT is available
 */
bool spectrum_is_supported(void);
