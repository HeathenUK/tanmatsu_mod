#include "mod_backend_openmpt.h"

// Only compile this backend if MOD_BACKEND_OPENMPT is defined
#if defined(MOD_BACKEND_OPENMPT)

#include "mod_backend_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

// Include libopenmpt C API
#include "libopenmpt.h"
#include "libopenmpt_ext.h"

static const char *TAG = "mod_backend_openmpt";

// Clipping debug statistics (global for visibility across functions)
static uint32_t clip_count_pos = 0;
static uint32_t clip_count_neg = 0;
static float max_sample_seen = 0.0f;
static float min_sample_seen = 0.0f;
static uint32_t total_samples = 0;
static uint32_t last_log_samples = 0;
#define CLIP_LOG_INTERVAL 100000  // Log every 100k samples (~4.5 sec at 22050 Hz)

// Backend context structure
struct openmpt_backend {
    openmpt_module_ext *mod_ext;  // Extended interface for channel muting
    openmpt_module *mod;          // Standard interface (retrieved from mod_ext)
    openmpt_module_ext_interface_interactive *interactive;  // Interactive interface for muting
    uint32_t sample_rate;
    int format;  // OPENMPT_FORMAT_MONO or OPENMPT_FORMAT_STEREO
    bool is_loaded;
    bool is_playing;
    int32_t repeat_count;  // -1 = infinite, 0 = play once, n>0 = repeat n times
};

// Silent log function (libopenmpt requires a log function)
static void silent_log_func(const char *message, void *user) {
    // Do nothing - we don't want libopenmpt logging
    (void)message;
    (void)user;
}

openmpt_backend_t* openmpt_backend_create(void) {
    openmpt_backend_t *ctx = (openmpt_backend_t *)calloc(1, sizeof(openmpt_backend_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate backend context");
        return NULL;
    }

    if (float_buffer == NULL) {
        float_buffer = (float *)heap_caps_malloc(MAX_RENDER_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
        if (float_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate OpenMPT float buffer in PSRAM");
            free(ctx);
            return NULL;
        }
    }
    
    ctx->mod_ext = NULL;
    ctx->mod = NULL;
    ctx->interactive = NULL;
    ctx->sample_rate = MOD_CONFIG_SAMPLE_RATE;
    ctx->format = OPENMPT_FORMAT_MONO;
    ctx->is_loaded = false;
    ctx->is_playing = false;
    ctx->repeat_count = -1;  // Default to infinite loop
    
    return ctx;
}

void openmpt_backend_free(openmpt_backend_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    
    if (ctx->mod_ext != NULL) {
        openmpt_module_ext_destroy(ctx->mod_ext);
        ctx->mod_ext = NULL;
        ctx->mod = NULL;
        ctx->interactive = NULL;
    }
    
    free(ctx);
}

esp_err_t openmpt_backend_load_module(openmpt_backend_t *ctx, const uint8_t *mod_data, size_t mod_size) {
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Release existing module if any
    if (ctx->mod_ext != NULL) {
        openmpt_module_ext_destroy(ctx->mod_ext);
        ctx->mod_ext = NULL;
        ctx->mod = NULL;
        ctx->interactive = NULL;
        ctx->is_loaded = false;
    }
    
    // Create extended module from memory (for channel muting support)
    int error = 0;
    const char *error_message = NULL;

    // Initial CTLs for faster loading on embedded systems
    // - skip_subsongs_init: Don't pre-calculate subsong info (main speedup)
    // - skip_plugins: Don't load VST plugins (not used in tracker formats)
    // NOTE: We do NOT skip samples - samples are required for playback and libopenmpt
    // does not support lazy loading samples on-demand during playback
    static const openmpt_module_initial_ctl init_ctls[] = {
        { "load.skip_subsongs_init", "1" },
        { "load.skip_plugins", "1" },
        { NULL, NULL }  // Terminator
    };

    ctx->mod_ext = openmpt_module_ext_create_from_memory(
        mod_data,
        mod_size,
        silent_log_func,  // Log function
        NULL,             // Log user data
        openmpt_error_func_ignore,  // Error function (ignore errors)
        NULL,             // Error user data
        &error,           // Error output
        &error_message,   // Error message output
        init_ctls         // Initial CTLs for faster loading
    );
    
    if (ctx->mod_ext == NULL) {
        ESP_LOGE(TAG, "Failed to load module (error: %d)", error);
        if (error_message != NULL) {
            ESP_LOGE(TAG, "Error message: %s", error_message);
            openmpt_free_string(error_message);
        }
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get standard module handle from extended module
    ctx->mod = openmpt_module_ext_get_module(ctx->mod_ext);
    if (ctx->mod == NULL) {
        ESP_LOGE(TAG, "Failed to get module handle from extended module");
        openmpt_module_ext_destroy(ctx->mod_ext);
        ctx->mod_ext = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get interactive interface for channel muting
    ctx->interactive = (openmpt_module_ext_interface_interactive *)calloc(1, sizeof(openmpt_module_ext_interface_interactive));
    if (ctx->interactive == NULL) {
        ESP_LOGE(TAG, "Failed to allocate interactive interface");
        openmpt_module_ext_destroy(ctx->mod_ext);
        ctx->mod_ext = NULL;
        ctx->mod = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    if (openmpt_module_ext_get_interface(ctx->mod_ext, LIBOPENMPT_EXT_C_INTERFACE_INTERACTIVE,
                                         ctx->interactive, sizeof(openmpt_module_ext_interface_interactive)) == 0) {
        ESP_LOGW(TAG, "Interactive interface not available - channel muting will not work");
        free(ctx->interactive);
        ctx->interactive = NULL;
    }
    
    // Set repeat count (default to infinite loop)
    openmpt_module_set_repeat_count(ctx->mod, ctx->repeat_count);
    
    ctx->is_loaded = true;
    ESP_LOGI(TAG, "Module loaded successfully (%zu bytes)", mod_size);

    // Reset clipping stats for new module
    clip_count_pos = 0;
    clip_count_neg = 0;
    max_sample_seen = 0.0f;
    min_sample_seen = 0.0f;
    total_samples = 0;
    last_log_samples = 0;

    return ESP_OK;
}

void openmpt_backend_release_module(openmpt_backend_t *ctx) {
    if (ctx == NULL || ctx->mod_ext == NULL) {
        return;
    }
    
    if (ctx->interactive != NULL) {
        free(ctx->interactive);
        ctx->interactive = NULL;
    }
    
    openmpt_module_ext_destroy(ctx->mod_ext);
    ctx->mod_ext = NULL;
    ctx->mod = NULL;
    ctx->is_loaded = false;
    ctx->is_playing = false;
}

esp_err_t openmpt_backend_start_player(openmpt_backend_t *ctx, uint32_t sample_rate, int format) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ctx->sample_rate = sample_rate;
    ctx->format = format;
    
    // Configure render parameters
    // Set interpolation filter length (2 = linear, good balance of quality/speed for embedded)
    // 1 = nearest (fastest), 2 = linear, 4 = cubic, 8 = sinc (slowest)
    openmpt_module_set_render_param(ctx->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, 2);
    
    // Set repeat count
    openmpt_module_set_render_param(ctx->mod, OPENMPT_MODULE_RENDER_VOLUMERAMPING_STRENGTH, -1);  // Default
    
    // Reset position to start
    openmpt_module_set_position_order_row(ctx->mod, 0, 0);
    
    ctx->is_playing = true;
    ESP_LOGI(TAG, "Player started at %lu Hz, format: %s", sample_rate, 
             (format == OPENMPT_FORMAT_MONO) ? "MONO" : "STEREO");
    
    return ESP_OK;
}

void openmpt_backend_end_player(openmpt_backend_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    
    ctx->is_playing = false;
}

#define MAX_RENDER_SAMPLES 4096
static float *float_buffer = NULL;

int openmpt_backend_play_buffer(openmpt_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_playing) {
        return -1;
    }
    if (float_buffer == NULL) {
        ESP_LOGE(TAG, "OpenMPT float buffer not available");
        return -1;
    }

    // Calculate number of samples (buffer_size is in bytes, int16_t is 2 bytes)
    size_t num_samples = buffer_size / sizeof(int16_t);
    if (num_samples > MAX_RENDER_SAMPLES) {
        num_samples = MAX_RENDER_SAMPLES;
    }

    // Set repeat count based on loop parameter
    int32_t repeat = (loop != 0) ? -1 : 0;
    if (ctx->repeat_count != repeat) {
        ctx->repeat_count = repeat;
        openmpt_module_set_repeat_count(ctx->mod, repeat);
    }

    // Render audio using float API (faster on FPU-equipped systems)
    // libopenmpt internally uses float, so this avoids double conversion
    size_t frames_rendered = openmpt_module_read_float_mono(ctx->mod, ctx->sample_rate, num_samples, float_buffer);

    // Convert float [-1.0, 1.0] to int16 with clipping detection
    for (size_t i = 0; i < frames_rendered; i++) {
        float raw_sample = float_buffer[i];

        // Track min/max for debugging
        if (raw_sample > max_sample_seen) max_sample_seen = raw_sample;
        if (raw_sample < min_sample_seen) min_sample_seen = raw_sample;

        float sample = raw_sample * 32767.0f;
        if (sample > 32767.0f) {
            sample = 32767.0f;
            clip_count_pos++;
        } else if (sample < -32768.0f) {
            sample = -32768.0f;
            clip_count_neg++;
        }
        buffer[i] = (int16_t)sample;
    }

    // Zero-pad remaining buffer if partial frame was rendered
    // This prevents playback of uninitialized/old data that could cause stuttering
    if (frames_rendered < num_samples) {
        memset(&buffer[frames_rendered], 0, (num_samples - frames_rendered) * sizeof(int16_t));
    }

    total_samples += frames_rendered;

    // Periodic logging of clipping stats
    if (total_samples - last_log_samples >= CLIP_LOG_INTERVAL) {
        if (clip_count_pos > 0 || clip_count_neg > 0) {
            ESP_LOGW(TAG, "CLIPPING: +%lu -%lu clips in %lu samples (range: %.3f to %.3f)",
                     clip_count_pos, clip_count_neg, total_samples,
                     min_sample_seen, max_sample_seen);
        } else {
            ESP_LOGI(TAG, "Audio OK: %lu samples, peak range: %.3f to %.3f",
                     total_samples, min_sample_seen, max_sample_seen);
        }
        last_log_samples = total_samples;
    }

    // Handle end of module
    if (frames_rendered == 0) {
        // When looping is enabled, libopenmpt will restart on the next call
        // Zero-fill the buffer and return 0 to allow the next call to restart the loop
        // When looping is disabled, return -1 to stop playback
        if (loop != 0) {
            // Looping enabled: zero-fill buffer and continue (next call will restart)
            memset(buffer, 0, num_samples * sizeof(int16_t));
            return 0;
        } else {
            // Looping disabled: signal end of playback
            return -1;
        }
    }

    // Success: more data available
    return 0;
}

esp_err_t openmpt_backend_get_frame_info(openmpt_backend_t *ctx, openmpt_frame_info_t *frame_info) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || frame_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Note: frame_info can be called even when not playing for UI display
    
    // Get current playback position
    frame_info->pos = openmpt_module_get_current_order(ctx->mod);
    frame_info->pattern = openmpt_module_get_current_pattern(ctx->mod);
    frame_info->row = openmpt_module_get_current_row(ctx->mod);
    frame_info->speed = openmpt_module_get_current_speed(ctx->mod);
    
    // Get BPM (convert from tempo)
    double tempo = openmpt_module_get_current_tempo2(ctx->mod);
    frame_info->bpm = (int)(tempo * 2.5);  // Rough conversion (libxmp uses different units)
    
    // Calculate frame time (microseconds per frame at current sample rate)
    frame_info->frame_time = (1000000.0f / ctx->sample_rate);
    
    // Get number of channels
    int num_channels = openmpt_module_get_num_channels(ctx->mod);
    
    // Limit to max channels
    if (num_channels > OPENMPT_MAX_CHANNELS) {
        num_channels = OPENMPT_MAX_CHANNELS;
    }
    
    // Store num_channels in frame_info (required for UI display)
    frame_info->num_channels = num_channels;
    
    // Get channel info for each channel
    for (int ch = 0; ch < num_channels; ch++) {
        // Get pattern row channel data
        uint8_t note = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, frame_info->pattern, frame_info->row, ch, OPENMPT_MODULE_COMMAND_NOTE);
        uint8_t ins = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, frame_info->pattern, frame_info->row, ch, OPENMPT_MODULE_COMMAND_INSTRUMENT);
        uint8_t fxt = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, frame_info->pattern, frame_info->row, ch, OPENMPT_MODULE_COMMAND_EFFECT);
        uint8_t fxp = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, frame_info->pattern, frame_info->row, ch, OPENMPT_MODULE_COMMAND_PARAMETER);
        
        // Map to channel_info structure (note the 'event' nested struct)
        frame_info->channel_info[ch].event.note = note;
        frame_info->channel_info[ch].event.ins = ins;
        frame_info->channel_info[ch].event.fxt = fxt;
        frame_info->channel_info[ch].event.fxp = fxp;

        // Get channel volume for VU meter display
        // libopenmpt provides volume in 0.0-1.0 range, convert to 0-64
        double vol = openmpt_module_get_current_channel_vu_mono(ctx->mod, ch);
        frame_info->channel_info[ch].volume = (uint8_t)(vol * 64.0);
        frame_info->channel_info[ch].period = 0;  // libopenmpt doesn't expose period directly
    }
    
    // Zero out remaining channels to prevent garbage data
    for (int ch = num_channels; ch < OPENMPT_MAX_CHANNELS; ch++) {
        frame_info->channel_info[ch].event.note = 0;
        frame_info->channel_info[ch].event.ins = 0;
        frame_info->channel_info[ch].event.fxt = 0;
        frame_info->channel_info[ch].event.fxp = 0;
        frame_info->channel_info[ch].volume = 0;
        frame_info->channel_info[ch].period = 0;
    }
    
    return ESP_OK;
}

esp_err_t openmpt_backend_get_module_info(openmpt_backend_t *ctx, openmpt_module_info_t *mod_info) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || mod_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get module name from metadata
    const char *title = openmpt_module_get_metadata(ctx->mod, "title");
    mod_info->name = (title != NULL && strlen(title) > 0) ? title : "";
    
    // Get number of channels
    mod_info->chn = openmpt_module_get_num_channels(ctx->mod);
    
    // Get number of patterns
    mod_info->pat = openmpt_module_get_num_patterns(ctx->mod);
    
    return ESP_OK;
}

esp_err_t openmpt_backend_set_player(openmpt_backend_t *ctx, int param, int value) {
    if (ctx == NULL || ctx->mod == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    switch (param) {
        case OPENMPT_PLAYER_INTERP:
            // Map interpolation mode to libopenmpt filter length
            // 0 = nearest, 1 = linear, 2 = cubic, 3 = sinc
            int filter_length = 1;  // Default to no interpolation
            switch (value) {
                case OPENMPT_INTERP_NEAREST:
                    filter_length = 1;
                    break;
                case OPENMPT_INTERP_LINEAR:
                    filter_length = 2;
                    break;
                case OPENMPT_INTERP_CUBIC:
                    filter_length = 4;
                    break;
                case OPENMPT_INTERP_SINC:
                    filter_length = 8;
                    break;
                default:
                    filter_length = 8;  // Default to highest quality
                    break;
            }
            if (openmpt_module_set_render_param(ctx->mod, OPENMPT_MODULE_RENDER_INTERPOLATIONFILTER_LENGTH, filter_length) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            break;
            
        case OPENMPT_PLAYER_DSP:
            // DSP filtering is handled by libopenmpt internally
            // We can't directly disable it, but it's always enabled for quality
            break;
            
        case OPENMPT_PLAYER_VOICES:
            // libopenmpt doesn't have a direct voice limit setting
            // This is handled internally
            break;
            
        default:
            return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
}

int openmpt_backend_channel_mute(openmpt_backend_t *ctx, int channel, int status) {
    if (ctx == NULL || ctx->mod == NULL) {
        return -1;
    }
    
    int num_channels = openmpt_module_get_num_channels(ctx->mod);
    if (channel < 0 || channel >= num_channels) {
        return -1;
    }
    
    // Check if interactive interface is available
    if (ctx->interactive == NULL || ctx->interactive->set_channel_mute_status == NULL ||
        ctx->interactive->get_channel_mute_status == NULL) {
        ESP_LOGW(TAG, "Interactive interface not available for channel muting");
        return -1;
    }
    
    // Query current state (status == -1)
    if (status == -1) {
        int mute_status = ctx->interactive->get_channel_mute_status(ctx->mod_ext, channel);
        return mute_status;  // Returns 1 if muted, 0 if unmuted, -1 on error
    }
    
    // Set mute state (status == 0 unmute, 1 mute, 2 toggle)
    if (status == 2) {
        // Toggle: get current state and flip it
        int current = ctx->interactive->get_channel_mute_status(ctx->mod_ext, channel);
        if (current < 0) {
            return -1;  // Error querying
        }
        status = (current == 0) ? 1 : 0;  // Flip state
    }
    
    // Set mute status (status == 0 unmute, 1 mute)
    int result = ctx->interactive->set_channel_mute_status(ctx->mod_ext, channel, status);
    if (result == 0) {
        return -1;  // Failed to set
    }
    
    // Return new state
    return status;
}

esp_err_t openmpt_backend_get_pattern_row_channel(openmpt_backend_t *ctx, 
                                                   int pattern, int row, int channel,
                                                   uint8_t *note, uint8_t *ins, 
                                                   uint8_t *fxt, uint8_t *fxp) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int num_patterns = openmpt_module_get_num_patterns(ctx->mod);
    if (pattern < 0 || pattern >= num_patterns) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int num_rows = openmpt_module_get_pattern_num_rows(ctx->mod, pattern);
    if (row < 0 || row >= num_rows) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int num_channels = openmpt_module_get_num_channels(ctx->mod);
    if (channel < 0 || channel >= num_channels) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (note != NULL) {
        *note = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, pattern, row, channel, OPENMPT_MODULE_COMMAND_NOTE);
    }
    
    if (ins != NULL) {
        *ins = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, pattern, row, channel, OPENMPT_MODULE_COMMAND_INSTRUMENT);
    }
    
    if (fxt != NULL) {
        *fxt = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, pattern, row, channel, OPENMPT_MODULE_COMMAND_EFFECT);
    }
    
    if (fxp != NULL) {
        *fxp = openmpt_module_get_pattern_row_channel_command(
            ctx->mod, pattern, row, channel, OPENMPT_MODULE_COMMAND_PARAMETER);
    }
    
    return ESP_OK;
}

esp_err_t openmpt_backend_get_pattern_num_rows(openmpt_backend_t *ctx, int pattern, int *num_rows) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || num_rows == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int num_patterns = openmpt_module_get_num_patterns(ctx->mod);
    if (pattern < 0 || pattern >= num_patterns) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int32_t rows = openmpt_module_get_pattern_num_rows(ctx->mod, pattern);
    if (rows <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *num_rows = rows;
    return ESP_OK;
}

esp_err_t openmpt_backend_get_order_pattern(openmpt_backend_t *ctx, int order, int *pattern) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || pattern == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Get number of orders
    int num_orders = openmpt_module_get_num_orders(ctx->mod);
    if (order < 0 || order >= num_orders) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get pattern number for this order position
    int32_t pat = openmpt_module_get_order_pattern(ctx->mod, order);
    if (pat < 0) {
        // Invalid pattern (skip or stop entry)
        return ESP_ERR_INVALID_STATE;
    }
    
    *pattern = pat;
    return ESP_OK;
}

bool openmpt_backend_is_loaded(openmpt_backend_t *ctx) {
    return (ctx != NULL && ctx->is_loaded);
}

bool openmpt_backend_is_playing(openmpt_backend_t *ctx) {
    return (ctx != NULL && ctx->is_playing && ctx->is_loaded);
}

#else // MOD_BACKEND_OPENMPT not defined - provide stub implementations

// Stub implementations when OPENMPT backend is not selected
// These will never be called, but prevent linker errors

openmpt_backend_t* openmpt_backend_create(void) { return NULL; }
void openmpt_backend_free(openmpt_backend_t *ctx) { (void)ctx; }
esp_err_t openmpt_backend_load_module(openmpt_backend_t *ctx, const uint8_t *mod_data, size_t mod_size) { (void)ctx; (void)mod_data; (void)mod_size; return ESP_ERR_NOT_SUPPORTED; }
void openmpt_backend_release_module(openmpt_backend_t *ctx) { (void)ctx; }
esp_err_t openmpt_backend_start_player(openmpt_backend_t *ctx, uint32_t sample_rate, int format) { (void)ctx; (void)sample_rate; (void)format; return ESP_ERR_NOT_SUPPORTED; }
void openmpt_backend_end_player(openmpt_backend_t *ctx) { (void)ctx; }
int openmpt_backend_play_buffer(openmpt_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop) { (void)ctx; (void)buffer; (void)buffer_size; (void)loop; return -1; }
esp_err_t openmpt_backend_get_frame_info(openmpt_backend_t *ctx, openmpt_frame_info_t *frame_info) { (void)ctx; (void)frame_info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t openmpt_backend_get_module_info(openmpt_backend_t *ctx, openmpt_module_info_t *mod_info) { (void)ctx; (void)mod_info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t openmpt_backend_set_player(openmpt_backend_t *ctx, int param, int value) { (void)ctx; (void)param; (void)value; return ESP_ERR_NOT_SUPPORTED; }
int openmpt_backend_channel_mute(openmpt_backend_t *ctx, int channel, int status) { (void)ctx; (void)channel; (void)status; return -1; }
esp_err_t openmpt_backend_get_pattern_row_channel(openmpt_backend_t *ctx, int pattern, int row, int channel, uint8_t *note, uint8_t *ins, uint8_t *fxt, uint8_t *fxp) { (void)ctx; (void)pattern; (void)row; (void)channel; (void)note; (void)ins; (void)fxt; (void)fxp; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t openmpt_backend_get_pattern_num_rows(openmpt_backend_t *ctx, int pattern, int *num_rows) { (void)ctx; (void)pattern; (void)num_rows; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t openmpt_backend_get_order_pattern(openmpt_backend_t *ctx, int order, int *pattern) { (void)ctx; (void)order; (void)pattern; return ESP_ERR_NOT_SUPPORTED; }
bool openmpt_backend_is_loaded(openmpt_backend_t *ctx) { (void)ctx; return false; }
bool openmpt_backend_is_playing(openmpt_backend_t *ctx) { (void)ctx; return false; }

#endif // MOD_BACKEND_OPENMPT
