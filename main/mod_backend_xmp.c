#include "mod_backend_xmp.h"

// Only compile this backend if MOD_BACKEND_XMP is defined
#if defined(MOD_BACKEND_XMP)

#include "mod_backend_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

// Include libxmp C API
#include "xmp.h"

static const char *TAG = "mod_backend_xmp";

// Backend context structure
struct xmp_backend {
    xmp_context ctx;        // libxmp context
    uint32_t sample_rate;
    int format;             // XMP_BACKEND_FORMAT_MONO or XMP_BACKEND_FORMAT_STEREO
    bool is_loaded;
    bool is_playing;
};

xmp_backend_t* xmp_backend_create(void) {
    xmp_backend_t *backend = (xmp_backend_t *)calloc(1, sizeof(xmp_backend_t));
    if (backend == NULL) {
        ESP_LOGE(TAG, "Failed to allocate backend context");
        return NULL;
    }
    
    backend->ctx = xmp_create_context();
    if (backend->ctx == NULL) {
        ESP_LOGE(TAG, "Failed to create libxmp context");
        free(backend);
        return NULL;
    }
    
    backend->sample_rate = MOD_CONFIG_SAMPLE_RATE;
    backend->format = XMP_BACKEND_FORMAT_MONO;
    backend->is_loaded = false;
    backend->is_playing = false;
    
    return backend;
}

void xmp_backend_free(xmp_backend_t *backend) {
    if (backend == NULL) {
        return;
    }
    
    if (backend->ctx != NULL) {
        if (backend->is_loaded) {
            xmp_release_module(backend->ctx);
        }
        if (backend->is_playing) {
            xmp_end_player(backend->ctx);
        }
        xmp_free_context(backend->ctx);
        backend->ctx = NULL;
    }
    
    free(backend);
}

esp_err_t xmp_backend_load_module(xmp_backend_t *backend, const uint8_t *mod_data, size_t mod_size) {
    if (backend == NULL || backend->ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Release existing module if any
    if (backend->is_loaded) {
        xmp_release_module(backend->ctx);
        backend->is_loaded = false;
        backend->is_playing = false;
    }
    
    // Load module from memory
    int ret = xmp_load_module_from_memory(backend->ctx, mod_data, (long)mod_size);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load module (error: %d)", ret);
        return ESP_ERR_INVALID_ARG;
    }
    
    backend->is_loaded = true;
    ESP_LOGI(TAG, "Module loaded successfully (%zu bytes)", mod_size);
    
    return ESP_OK;
}

void xmp_backend_release_module(xmp_backend_t *backend) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded) {
        return;
    }
    
    if (backend->is_playing) {
        xmp_end_player(backend->ctx);
        backend->is_playing = false;
    }
    
    xmp_release_module(backend->ctx);
    backend->is_loaded = false;
}

esp_err_t xmp_backend_start_player(xmp_backend_t *backend, uint32_t sample_rate, int format) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Stop player if already playing
    if (backend->is_playing) {
        xmp_end_player(backend->ctx);
        backend->is_playing = false;
    }
    
    backend->sample_rate = sample_rate;
    backend->format = format;
    
    // Start player with format flags
    // libxmp format: XMP_FORMAT_MONO = (1 << 2), XMP_FORMAT_STEREO = 0
    int xmp_format = (format == XMP_BACKEND_FORMAT_MONO) ? XMP_FORMAT_MONO : 0;
    int ret = xmp_start_player(backend->ctx, (int)sample_rate, xmp_format);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to start player (error: %d)", ret);
        return ESP_ERR_INVALID_STATE;
    }
    
    backend->is_playing = true;
    ESP_LOGI(TAG, "Player started at %lu Hz, format: %s", sample_rate,
             (format == XMP_BACKEND_FORMAT_MONO) ? "MONO" : "STEREO");
    
    return ESP_OK;
}

void xmp_backend_end_player(xmp_backend_t *backend) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_playing) {
        return;
    }
    
    xmp_end_player(backend->ctx);
    backend->is_playing = false;
}

int xmp_backend_play_buffer(xmp_backend_t *backend, int16_t *buffer, size_t buffer_size, int loop) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_playing) {
        return -1;
    }
    
    // Calculate number of samples (buffer_size is in bytes, int16_t is 2 bytes)
    int num_samples = (int)(buffer_size / sizeof(int16_t));
    
    // Play buffer (libxmp handles looping internally based on loop parameter)
    // Note: loop parameter is not directly passed to xmp_play_buffer, but libxmp loops by default
    // We may need to check position and manually restart if loop is disabled
    int ret = xmp_play_buffer(backend->ctx, buffer, num_samples * sizeof(int16_t), loop);
    
    // xmp_play_buffer returns XMP_END (1) when end is reached, 0 on success
    if (ret == XMP_END) {
        return -1;  // End reached
    }
    
    return 0;  // Success, more data available
}

esp_err_t xmp_backend_get_frame_info(xmp_backend_t *backend, xmp_backend_frame_info_t *frame_info) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded || frame_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get frame info from libxmp
    struct xmp_frame_info xmp_frame;
    xmp_get_frame_info(backend->ctx, &xmp_frame);
    
    // Map libxmp frame info to our structure
    frame_info->pos = xmp_frame.pos;
    frame_info->pattern = xmp_frame.pattern;
    frame_info->row = xmp_frame.row;
    frame_info->speed = xmp_frame.speed;
    frame_info->bpm = xmp_frame.bpm;
    frame_info->frame_time = xmp_frame.frame_time;
    
    // Get number of channels from module info
    struct xmp_module_info mod_info;
    xmp_get_module_info(backend->ctx, &mod_info);
    frame_info->num_channels = mod_info.mod->chn;
    
    // Limit to max channels
    if (frame_info->num_channels > XMP_BACKEND_MAX_CHANNELS) {
        frame_info->num_channels = XMP_BACKEND_MAX_CHANNELS;
    }
    
    // Copy channel info
    for (int ch = 0; ch < frame_info->num_channels && ch < XMP_MAX_CHANNELS; ch++) {
        frame_info->channel_info[ch].event.note = xmp_frame.channel_info[ch].event.note;
        frame_info->channel_info[ch].event.ins = xmp_frame.channel_info[ch].event.ins;
        frame_info->channel_info[ch].event.fxt = xmp_frame.channel_info[ch].event.fxt;
        frame_info->channel_info[ch].event.fxp = xmp_frame.channel_info[ch].event.fxp;
    }
    
    return ESP_OK;
}

esp_err_t xmp_backend_get_module_info(xmp_backend_t *backend, xmp_backend_module_info_t *mod_info) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded || mod_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get module info from libxmp
    struct xmp_module_info xmp_mod_info;
    xmp_get_module_info(backend->ctx, &xmp_mod_info);
    
    // Map libxmp module info to our structure
    // Note: xmp_mod_info.mod->name is a char array, not a pointer
    mod_info->name = xmp_mod_info.mod->name;
    mod_info->chn = xmp_mod_info.mod->chn;
    mod_info->pat = xmp_mod_info.mod->pat;
    
    return ESP_OK;
}

esp_err_t xmp_backend_set_player(xmp_backend_t *backend, int param, int value) {
    if (backend == NULL || backend->ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int xmp_param = 0;
    int xmp_value = value;
    
    // Map our parameter constants to libxmp constants
    switch (param) {
        case XMP_BACKEND_PLAYER_AMP:
            xmp_param = XMP_PLAYER_AMP;
            // Clamp to valid range 0-3
            if (value < 0) xmp_value = 0;
            else if (value > 3) xmp_value = 3;
            break;

        case XMP_BACKEND_PLAYER_INTERP:
            xmp_param = XMP_PLAYER_INTERP;
            // Map interpolation modes
            // XMP_INTERP_NEAREST=0, XMP_INTERP_LINEAR=1, XMP_INTERP_SPLINE=2
            // Our: NEAREST=0, LINEAR=1, SPLINE=2, CUBIC=3
            if (value == XMP_BACKEND_INTERP_CUBIC) {
                xmp_value = XMP_INTERP_SPLINE;  // Use spline as closest match
            } else if (value >= 0 && value <= 2) {
                // Direct mapping
            } else {
                xmp_value = XMP_INTERP_LINEAR;  // Default
            }
            break;
            
        case XMP_BACKEND_PLAYER_DSP:
            xmp_param = XMP_PLAYER_DSP;
            // Map DSP modes
            // XMP_DSP_LOWPASS = (1 << 0)
            if (value == XMP_BACKEND_DSP_LOWPASS) {
                xmp_value = XMP_DSP_LOWPASS;
            } else {
                xmp_value = 0;  // No DSP
            }
            break;
            
        case XMP_BACKEND_PLAYER_VOICES:
            xmp_param = XMP_PLAYER_VOICES;
            break;
            
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    int ret = xmp_set_player(backend->ctx, xmp_param, xmp_value);
    if (ret != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

int xmp_backend_channel_mute(xmp_backend_t *backend, int channel, int status) {
    if (backend == NULL || backend->ctx == NULL) {
        return -1;
    }
    
    // Get number of channels
    struct xmp_module_info mod_info;
    xmp_get_module_info(backend->ctx, &mod_info);
    int num_channels = mod_info.mod->chn;
    
    if (channel < 0 || channel >= num_channels) {
        return -1;
    }
    
    // Query current state (status == -1)
    if (status == -1) {
        int mute_status = xmp_channel_mute(backend->ctx, channel, -1);
        return mute_status;  // Returns 1 if muted, 0 if unmuted
    }
    
    // Set mute state (status == 0 unmute, 1 mute, 2 toggle)
    if (status == 2) {
        // Toggle: get current state and flip it
        int current = xmp_channel_mute(backend->ctx, channel, -1);
        if (current < 0) {
            return -1;  // Error querying
        }
        status = (current == 0) ? 1 : 0;  // Flip state
    }
    
    // Set mute status (status == 0 unmute, 1 mute)
    int result = xmp_channel_mute(backend->ctx, channel, status);
    if (result < 0) {
        return -1;  // Failed to set
    }
    
    // Return new state
    return status;
}

esp_err_t xmp_backend_get_pattern_row_channel(xmp_backend_t *backend, 
                                               int pattern, int row, int channel,
                                               uint8_t *note, uint8_t *ins, 
                                               uint8_t *fxt, uint8_t *fxp) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get module info to access pattern data
    struct xmp_module_info mod_info;
    xmp_get_module_info(backend->ctx, &mod_info);
    
    // Validate pattern number
    if (pattern < 0 || pattern >= mod_info.mod->pat) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate channel number
    int num_channels = mod_info.mod->chn;
    if (channel < 0 || channel >= num_channels) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Access pattern data directly via libxmp's internal structures
    struct xmp_pattern *pattern_ptr = mod_info.mod->xxp[pattern];
    if (pattern_ptr == NULL) {
        ESP_LOGE(TAG, "Pattern %d is NULL", pattern);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate row number
    if (row < 0 || row >= pattern_ptr->rows) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get track index for this channel in this pattern
    // The pattern->index array has 'chn' elements (one per channel)
    int track_index = pattern_ptr->index[channel];
    
    // Validate track index
    if (track_index < 0 || track_index >= mod_info.mod->trk) {
        ESP_LOGE(TAG, "Invalid track index %d for pattern %d, channel %d", track_index, pattern, channel);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get track pointer
    struct xmp_track *track = mod_info.mod->xxt[track_index];
    if (track == NULL) {
        ESP_LOGE(TAG, "Track %d is NULL", track_index);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Validate row is within track bounds
    if (row >= track->rows) {
        ESP_LOGE(TAG, "Row %d out of bounds for track %d (track has %d rows)", row, track_index, track->rows);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Access event at this row
    struct xmp_event *event = &track->event[row];
    
    // Extract values
    if (note != NULL) *note = event->note;
    if (ins != NULL) *ins = event->ins;
    if (fxt != NULL) *fxt = event->fxt;
    if (fxp != NULL) *fxp = event->fxp;
    
    return ESP_OK;
}

esp_err_t xmp_backend_get_pattern_num_rows(xmp_backend_t *backend, int pattern, int *num_rows) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded || num_rows == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get module info to access pattern data
    struct xmp_module_info mod_info;
    xmp_get_module_info(backend->ctx, &mod_info);
    
    // Validate pattern number
    if (pattern < 0 || pattern >= mod_info.mod->pat) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Access pattern data directly via libxmp's internal structures
    struct xmp_pattern *pattern_ptr = mod_info.mod->xxp[pattern];
    if (pattern_ptr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *num_rows = pattern_ptr->rows;
    return ESP_OK;
}

esp_err_t xmp_backend_get_order_pattern(xmp_backend_t *backend, int order, int *pattern) {
    if (backend == NULL || backend->ctx == NULL || !backend->is_loaded || pattern == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get module info to access order sequence
    struct xmp_module_info mod_info;
    xmp_get_module_info(backend->ctx, &mod_info);
    
    // Validate order number
    if (order < 0 || order >= mod_info.mod->len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Access order sequence directly via libxmp's internal structures
    // mod_info.mod->xxo[order] contains the pattern number for this order position
    *pattern = mod_info.mod->xxo[order];
    
    return ESP_OK;
}

bool xmp_backend_is_loaded(xmp_backend_t *backend) {
    return (backend != NULL && backend->is_loaded);
}

bool xmp_backend_is_playing(xmp_backend_t *backend) {
    return (backend != NULL && backend->is_playing && backend->is_loaded);
}

#else // MOD_BACKEND_XMP not defined - provide stub implementations

// Stub implementations when XMP backend is not selected
// These will never be called, but prevent linker errors

xmp_backend_t* xmp_backend_create(void) { return NULL; }
void xmp_backend_free(xmp_backend_t *backend) { (void)backend; }
esp_err_t xmp_backend_load_module(xmp_backend_t *backend, const uint8_t *mod_data, size_t mod_size) { (void)backend; (void)mod_data; (void)mod_size; return ESP_ERR_NOT_SUPPORTED; }
void xmp_backend_release_module(xmp_backend_t *backend) { (void)backend; }
esp_err_t xmp_backend_start_player(xmp_backend_t *backend, uint32_t sample_rate, int format) { (void)backend; (void)sample_rate; (void)format; return ESP_ERR_NOT_SUPPORTED; }
void xmp_backend_end_player(xmp_backend_t *backend) { (void)backend; }
int xmp_backend_play_buffer(xmp_backend_t *backend, int16_t *buffer, size_t buffer_size, int loop) { (void)backend; (void)buffer; (void)buffer_size; (void)loop; return -1; }
esp_err_t xmp_backend_get_frame_info(xmp_backend_t *backend, xmp_backend_frame_info_t *frame_info) { (void)backend; (void)frame_info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xmp_backend_get_module_info(xmp_backend_t *backend, xmp_backend_module_info_t *mod_info) { (void)backend; (void)mod_info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xmp_backend_set_player(xmp_backend_t *backend, int param, int value) { (void)backend; (void)param; (void)value; return ESP_ERR_NOT_SUPPORTED; }
int xmp_backend_channel_mute(xmp_backend_t *backend, int channel, int status) { (void)backend; (void)channel; (void)status; return -1; }
esp_err_t xmp_backend_get_pattern_row_channel(xmp_backend_t *backend, int pattern, int row, int channel, uint8_t *note, uint8_t *ins, uint8_t *fxt, uint8_t *fxp) { (void)backend; (void)pattern; (void)row; (void)channel; (void)note; (void)ins; (void)fxt; (void)fxp; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xmp_backend_get_pattern_num_rows(xmp_backend_t *backend, int pattern, int *num_rows) { (void)backend; (void)pattern; (void)num_rows; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t xmp_backend_get_order_pattern(xmp_backend_t *backend, int order, int *pattern) { (void)backend; (void)order; (void)pattern; return ESP_ERR_NOT_SUPPORTED; }
bool xmp_backend_is_loaded(xmp_backend_t *backend) { (void)backend; return false; }
bool xmp_backend_is_playing(xmp_backend_t *backend) { (void)backend; return false; }

#endif // MOD_BACKEND_XMP
