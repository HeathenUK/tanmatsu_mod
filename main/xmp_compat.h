#pragma once

/**
 * @file xmp_compat.h
 * @brief Compatibility header for libxmp structures
 * 
 * This header provides the structure definitions needed for compatibility
 * with libxmp, allowing the UI code to work without libxmp being present.
 */

#include <stdint.h>

// Compatibility constants
#define XMP_MAX_CHANNELS 64
#define XMP_MAX_MOD_LENGTH 256

// Forward declarations
struct xmp_module;
struct xmp_channel_info;
struct xmp_frame_info;
struct xmp_module_info;

// Compatibility structures matching libxmp
struct xmp_event {
    uint8_t note;   // Note (1-96, 0 = no note)
    uint8_t ins;    // Instrument (1-based, 0 = no instrument)
    uint8_t fxt;    // Effect type (0-255)
    uint8_t fxp;    // Effect parameter (0-255)
};

struct xmp_channel_info {
    struct xmp_event event;
    uint8_t volume;    // Channel volume (0-64)
    uint16_t period;   // Note period (for pitch display)
};

struct xmp_frame_info {
    int pos;           // Order/sequence position
    int pattern;       // Current pattern index
    int row;           // Current row within pattern
    int speed;         // Speed (ticks per row)
    int bpm;           // Beats per minute
    float frame_time;  // Frame time in microseconds
    int num_channels;  // Number of channels
    struct xmp_channel_info channel_info[XMP_MAX_CHANNELS];
};

struct xmp_module {
    char name[64];    // Module name
    char type[64];    // Module format
    int pat;          // Number of patterns
    int trk;          // Number of tracks
    int chn;          // Tracks per pattern (number of channels)
    int ins;          // Number of instruments
    int smp;          // Number of samples
    int spd;          // Initial speed
    int bpm;          // Initial BPM
    int len;          // Module length in patterns
    int rst;          // Restart position
    int gvl;          // Global volume
};

struct xmp_module_info {
    unsigned char md5[16];  // MD5 message digest
    struct xmp_module *mod; // Pointer to module data
    // Add other fields as needed
};
