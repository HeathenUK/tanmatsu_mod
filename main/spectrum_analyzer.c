#include "spectrum_analyzer.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

// ESP-IDF DSP library for hardware-accelerated FFT
#include "dsps_fft2r.h"
#include "dsps_wind.h"

static const char *TAG = "spectrum";

// =============================================================================
// Internal Structures
// =============================================================================

struct spectrum_analyzer {
    // Configuration
    uint32_t sample_rate;
    float decay_rate;
    float peak_decay_rate;
    uint32_t peak_hold_frames;

    // Sample ring buffer
    int16_t *ring_buffer;
    size_t ring_size;
    size_t ring_write_pos;
    size_t ring_samples_count;

    // FFT buffers (aligned for SIMD)
    float *fft_input;     // FFT input (real values)
    float *fft_output;    // FFT output (complex interleaved: re, im, re, im, ...)
    float *window;        // Hanning window coefficients

    // Output levels
    float *band_levels;   // Current smoothed band levels (0.0-1.0)
    float *peak_levels;   // Peak hold levels (0.0-1.0)
    uint32_t *peak_hold_counters;  // Frames remaining for peak hold

    // Band mapping (FFT bin to band)
    int *band_bin_start;  // Starting FFT bin for each band
    int *band_bin_end;    // Ending FFT bin for each band

    // State
    bool initialized;
};

// =============================================================================
// Logarithmic band mapping
// =============================================================================

// Pre-compute bin ranges for logarithmic frequency bands
// This maps FFT bins to display bands with log spacing
static void compute_band_mapping(spectrum_analyzer_t *analyzer) {
    int fft_bins = SPECTRUM_FFT_SIZE / 2;  // Usable bins (0 to Nyquist)
    int num_bands = SPECTRUM_NUM_BANDS;

    // Use logarithmic scaling for more musically relevant bands
    // Low frequencies get fewer bins, high frequencies get more bins combined
    float log_min = log2f(1);
    float log_max = log2f(fft_bins);
    float log_range = log_max - log_min;

    for (int band = 0; band < num_bands; band++) {
        // Logarithmic interpolation
        float t_start = (float)band / (float)num_bands;
        float t_end = (float)(band + 1) / (float)num_bands;

        int bin_start = (int)(pow(2.0f, log_min + t_start * log_range));
        int bin_end = (int)(pow(2.0f, log_min + t_end * log_range));

        // Ensure at least 1 bin per band
        if (bin_end <= bin_start) {
            bin_end = bin_start + 1;
        }

        // Clamp to valid range
        if (bin_start < 1) bin_start = 1;  // Skip DC bin
        if (bin_end > fft_bins) bin_end = fft_bins;

        analyzer->band_bin_start[band] = bin_start;
        analyzer->band_bin_end[band] = bin_end;
    }
}

// =============================================================================
// API Implementation
// =============================================================================

spectrum_analyzer_t *spectrum_init(const spectrum_config_t *config) {
    spectrum_analyzer_t *analyzer = calloc(1, sizeof(spectrum_analyzer_t));
    if (!analyzer) {
        ESP_LOGE(TAG, "Failed to allocate analyzer context");
        return NULL;
    }

    // Use default config if not provided
    spectrum_config_t default_config = SPECTRUM_CONFIG_DEFAULT();
    if (!config) {
        config = &default_config;
    }

    analyzer->sample_rate = config->sample_rate;
    analyzer->decay_rate = config->decay_rate;
    analyzer->peak_decay_rate = config->peak_decay_rate;
    analyzer->peak_hold_frames = config->peak_hold_frames;

    // Allocate ring buffer (2x FFT size for overlap)
    analyzer->ring_size = SPECTRUM_FFT_SIZE * 2;
    analyzer->ring_buffer = heap_caps_aligned_alloc(16, analyzer->ring_size * sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!analyzer->ring_buffer) {
        analyzer->ring_buffer = aligned_alloc(16, analyzer->ring_size * sizeof(int16_t));
    }
    if (!analyzer->ring_buffer) {
        ESP_LOGE(TAG, "Failed to allocate ring buffer");
        goto error;
    }
    memset(analyzer->ring_buffer, 0, analyzer->ring_size * sizeof(int16_t));

    // Allocate FFT buffers (16-byte aligned for SIMD)
    analyzer->fft_input = heap_caps_aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!analyzer->fft_input) {
        analyzer->fft_input = aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float));
    }

    analyzer->fft_output = heap_caps_aligned_alloc(16, SPECTRUM_FFT_SIZE * 2 * sizeof(float),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!analyzer->fft_output) {
        analyzer->fft_output = aligned_alloc(16, SPECTRUM_FFT_SIZE * 2 * sizeof(float));
    }

    analyzer->window = heap_caps_aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!analyzer->window) {
        analyzer->window = aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float));
    }

    if (!analyzer->fft_input || !analyzer->fft_output || !analyzer->window) {
        ESP_LOGE(TAG, "Failed to allocate FFT buffers");
        goto error;
    }

    // Initialize Hanning window
    dsps_wind_hann_f32(analyzer->window, SPECTRUM_FFT_SIZE);

    // Allocate output buffers
    analyzer->band_levels = calloc(SPECTRUM_NUM_BANDS, sizeof(float));
    analyzer->peak_levels = calloc(SPECTRUM_NUM_BANDS, sizeof(float));
    analyzer->peak_hold_counters = calloc(SPECTRUM_NUM_BANDS, sizeof(uint32_t));
    analyzer->band_bin_start = calloc(SPECTRUM_NUM_BANDS, sizeof(int));
    analyzer->band_bin_end = calloc(SPECTRUM_NUM_BANDS, sizeof(int));

    if (!analyzer->band_levels || !analyzer->peak_levels ||
        !analyzer->peak_hold_counters || !analyzer->band_bin_start ||
        !analyzer->band_bin_end) {
        ESP_LOGE(TAG, "Failed to allocate output buffers");
        goto error;
    }

    // Compute logarithmic band mapping
    compute_band_mapping(analyzer);

    // Initialize FFT tables
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, SPECTRUM_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize FFT: %s", esp_err_to_name(ret));
        goto error;
    }

    analyzer->initialized = true;
    ESP_LOGI(TAG, "Spectrum analyzer initialized (%d-point FFT, %d bands)",
             SPECTRUM_FFT_SIZE, SPECTRUM_NUM_BANDS);

    return analyzer;

error:
    spectrum_free(analyzer);
    return NULL;
}

void spectrum_free(spectrum_analyzer_t *analyzer) {
    if (!analyzer) return;

    if (analyzer->ring_buffer) free(analyzer->ring_buffer);
    if (analyzer->fft_input) free(analyzer->fft_input);
    if (analyzer->fft_output) free(analyzer->fft_output);
    if (analyzer->window) free(analyzer->window);
    if (analyzer->band_levels) free(analyzer->band_levels);
    if (analyzer->peak_levels) free(analyzer->peak_levels);
    if (analyzer->peak_hold_counters) free(analyzer->peak_hold_counters);
    if (analyzer->band_bin_start) free(analyzer->band_bin_start);
    if (analyzer->band_bin_end) free(analyzer->band_bin_end);

    free(analyzer);
}

void spectrum_process_samples(spectrum_analyzer_t *analyzer,
                              const int16_t *samples, size_t count) {
    if (!analyzer || !analyzer->initialized || !samples || count == 0) return;

    // Add samples to ring buffer
    for (size_t i = 0; i < count; i++) {
        analyzer->ring_buffer[analyzer->ring_write_pos] = samples[i];
        analyzer->ring_write_pos = (analyzer->ring_write_pos + 1) % analyzer->ring_size;
        if (analyzer->ring_samples_count < analyzer->ring_size) {
            analyzer->ring_samples_count++;
        }
    }

    // Process FFT when we have enough samples
    while (analyzer->ring_samples_count >= SPECTRUM_FFT_SIZE) {
        // Read samples from ring buffer, apply window
        size_t read_pos = (analyzer->ring_write_pos + analyzer->ring_size - SPECTRUM_FFT_SIZE) % analyzer->ring_size;

        for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
            // Convert int16 to float and apply window
            float sample = (float)analyzer->ring_buffer[(read_pos + i) % analyzer->ring_size] / 32768.0f;
            analyzer->fft_input[i] = sample * analyzer->window[i];
        }

        // Perform real-to-complex FFT
        // dsps_fft2r_fc32 expects complex input (interleaved re/im)
        // For real input, we convert to complex with imaginary = 0
        for (int i = SPECTRUM_FFT_SIZE - 1; i >= 0; i--) {
            analyzer->fft_output[i * 2] = analyzer->fft_input[i];  // Real
            analyzer->fft_output[i * 2 + 1] = 0.0f;                // Imaginary
        }

        dsps_fft2r_fc32(analyzer->fft_output, SPECTRUM_FFT_SIZE);

        // Bit-reverse reordering
        dsps_bit_rev_fc32(analyzer->fft_output, SPECTRUM_FFT_SIZE);

        // Calculate magnitude for each band
        for (int band = 0; band < SPECTRUM_NUM_BANDS; band++) {
            float band_energy = 0.0f;
            int bin_count = 0;

            for (int bin = analyzer->band_bin_start[band];
                 bin < analyzer->band_bin_end[band]; bin++) {
                float re = analyzer->fft_output[bin * 2];
                float im = analyzer->fft_output[bin * 2 + 1];
                float magnitude = sqrtf(re * re + im * im);
                band_energy += magnitude;
                bin_count++;
            }

            if (bin_count > 0) {
                band_energy /= (float)bin_count;
            }

            // Scale to 0-1 range (empirical scaling factor)
            // Multiply by 8 for better visibility with typical music
            float level = band_energy * 8.0f;
            if (level > 1.0f) level = 1.0f;

            // Apply to smoothed level with attack/decay
            // Fast attack, slow decay
            if (level > analyzer->band_levels[band]) {
                // Fast attack
                analyzer->band_levels[band] = level;
            } else {
                // Slow decay (handled in spectrum_update_decay)
            }

            // Update peak
            if (level > analyzer->peak_levels[band]) {
                analyzer->peak_levels[band] = level;
                analyzer->peak_hold_counters[band] = analyzer->peak_hold_frames;
            }
        }

        // We've processed FFT_SIZE samples, reduce count
        // Use 50% overlap for smoother animation
        analyzer->ring_samples_count -= SPECTRUM_FFT_SIZE / 2;
    }
}

void spectrum_get_bands(spectrum_analyzer_t *analyzer,
                        uint8_t *bands, size_t num_bands) {
    if (!analyzer || !bands) return;

    if (num_bands > SPECTRUM_NUM_BANDS) num_bands = SPECTRUM_NUM_BANDS;

    for (size_t i = 0; i < num_bands; i++) {
        bands[i] = (uint8_t)(analyzer->band_levels[i] * 255.0f);
    }
}

void spectrum_get_peaks(spectrum_analyzer_t *analyzer,
                        uint8_t *peaks, size_t num_bands) {
    if (!analyzer || !peaks) return;

    if (num_bands > SPECTRUM_NUM_BANDS) num_bands = SPECTRUM_NUM_BANDS;

    for (size_t i = 0; i < num_bands; i++) {
        peaks[i] = (uint8_t)(analyzer->peak_levels[i] * 255.0f);
    }
}

void spectrum_get_bands_and_peaks(spectrum_analyzer_t *analyzer,
                                   uint8_t *bands, uint8_t *peaks,
                                   size_t num_bands) {
    if (!analyzer) return;

    if (num_bands > SPECTRUM_NUM_BANDS) num_bands = SPECTRUM_NUM_BANDS;

    for (size_t i = 0; i < num_bands; i++) {
        if (bands) {
            bands[i] = (uint8_t)(analyzer->band_levels[i] * 255.0f);
        }
        if (peaks) {
            peaks[i] = (uint8_t)(analyzer->peak_levels[i] * 255.0f);
        }
    }
}

void spectrum_reset(spectrum_analyzer_t *analyzer) {
    if (!analyzer) return;

    memset(analyzer->ring_buffer, 0, analyzer->ring_size * sizeof(int16_t));
    analyzer->ring_write_pos = 0;
    analyzer->ring_samples_count = 0;

    memset(analyzer->band_levels, 0, SPECTRUM_NUM_BANDS * sizeof(float));
    memset(analyzer->peak_levels, 0, SPECTRUM_NUM_BANDS * sizeof(float));
    memset(analyzer->peak_hold_counters, 0, SPECTRUM_NUM_BANDS * sizeof(uint32_t));
}

void spectrum_update_decay(spectrum_analyzer_t *analyzer) {
    if (!analyzer) return;

    for (int i = 0; i < SPECTRUM_NUM_BANDS; i++) {
        // Decay band levels
        analyzer->band_levels[i] *= (1.0f - analyzer->decay_rate);
        if (analyzer->band_levels[i] < 0.01f) {
            analyzer->band_levels[i] = 0.0f;
        }

        // Decay peak levels (with hold)
        if (analyzer->peak_hold_counters[i] > 0) {
            analyzer->peak_hold_counters[i]--;
        } else {
            analyzer->peak_levels[i] *= (1.0f - analyzer->peak_decay_rate);
            if (analyzer->peak_levels[i] < 0.01f) {
                analyzer->peak_levels[i] = 0.0f;
            }
        }
    }
}

void spectrum_set_decay(spectrum_analyzer_t *analyzer,
                        float level_decay, float peak_decay) {
    if (!analyzer) return;

    if (level_decay >= 0.0f && level_decay <= 1.0f) {
        analyzer->decay_rate = level_decay;
    }
    if (peak_decay >= 0.0f && peak_decay <= 1.0f) {
        analyzer->peak_decay_rate = peak_decay;
    }
}

bool spectrum_is_supported(void) {
    // ESP32-P4 has PIE (Processor Instruction Extensions) for DSP
    // The dsps library automatically uses hardware acceleration when available
    return true;
}
