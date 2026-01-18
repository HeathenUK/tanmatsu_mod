#include "mod_backend_micromod.h"

// Only compile this backend if MOD_BACKEND_MICROMOD is defined
#if defined(MOD_BACKEND_MICROMOD)

#include "mod_backend_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

// Include IBXM library
#include "ibxm.h"

static const char *TAG = "mod_backend_micromod";

// Backend context structure
struct micromod_backend {
    struct module *mod;           // IBXM module
    struct replay *replay;        // IBXM replay context
    struct data mod_data;         // Module data (IBXM needs this to persist)
    uint32_t sample_rate;
    int format;                   // MICROMOD_FORMAT_MONO or MICROMOD_FORMAT_STEREO
    int interpolation;            // 0=none, 1=linear, 2=sinc
    bool is_loaded;
    bool is_playing;
    int32_t repeat_count;         // -1 = infinite, 0 = play once
    uint32_t mute_mask;           // Bitmask for muted channels
    int *mix_buffer;              // Mixing buffer for replay_get_audio
    int mix_buffer_len;           // Length of mix buffer in stereo samples
    int mix_buffer_pos;           // Current position in mix buffer
    int mix_buffer_avail;         // Samples available in mix buffer
    bool song_ended;              // Flag to track if song has ended
};

micromod_backend_t* micromod_backend_create(void) {
    micromod_backend_t *ctx = (micromod_backend_t *)calloc(1, sizeof(micromod_backend_t));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "Failed to allocate backend context");
        return NULL;
    }

    ctx->mod = NULL;
    ctx->replay = NULL;
    ctx->mod_data.buffer = NULL;
    ctx->mod_data.length = 0;
    ctx->sample_rate = MOD_CONFIG_SAMPLE_RATE;
    ctx->format = MICROMOD_FORMAT_MONO;
    ctx->interpolation = 1;  // Linear interpolation by default
    ctx->is_loaded = false;
    ctx->is_playing = false;
    ctx->repeat_count = -1;  // Default to infinite loop
    ctx->mute_mask = 0;
    ctx->mix_buffer = NULL;
    ctx->mix_buffer_len = 0;
    ctx->mix_buffer_pos = 0;
    ctx->mix_buffer_avail = 0;
    ctx->song_ended = false;

    return ctx;
}

void micromod_backend_free(micromod_backend_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    micromod_backend_release_module(ctx);
    free(ctx);
}

esp_err_t micromod_backend_load_module(micromod_backend_t *ctx, const uint8_t *mod_data, size_t mod_size) {
    if (ctx == NULL || mod_data == NULL || mod_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Release existing module if any
    micromod_backend_release_module(ctx);

    // Copy module data (IBXM requires the data buffer to persist)
    ctx->mod_data.buffer = (char *)heap_caps_malloc(mod_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->mod_data.buffer == NULL) {
        // Try internal RAM if PSRAM not available
        ctx->mod_data.buffer = (char *)malloc(mod_size);
        if (ctx->mod_data.buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate module data buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    memcpy(ctx->mod_data.buffer, mod_data, mod_size);
    ctx->mod_data.length = mod_size;

    // Load module
    char error_msg[64] = {0};
    ctx->mod = module_load(&ctx->mod_data, error_msg);
    if (ctx->mod == NULL) {
        ESP_LOGE(TAG, "Failed to load module: %s", error_msg);
        free(ctx->mod_data.buffer);
        ctx->mod_data.buffer = NULL;
        ctx->mod_data.length = 0;
        return ESP_ERR_INVALID_ARG;
    }

    ctx->is_loaded = true;
    ctx->mute_mask = 0;
    ctx->song_ended = false;

    ESP_LOGI(TAG, "Module loaded: '%s' (%d channels, %d patterns)",
             ctx->mod->name, ctx->mod->num_channels, ctx->mod->num_patterns);

    return ESP_OK;
}

void micromod_backend_release_module(micromod_backend_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    // Stop playback first
    micromod_backend_end_player(ctx);

    // Free replay
    if (ctx->replay != NULL) {
        dispose_replay(ctx->replay);
        ctx->replay = NULL;
    }

    // Free module
    if (ctx->mod != NULL) {
        dispose_module(ctx->mod);
        ctx->mod = NULL;
    }

    // Free module data buffer
    if (ctx->mod_data.buffer != NULL) {
        free(ctx->mod_data.buffer);
        ctx->mod_data.buffer = NULL;
        ctx->mod_data.length = 0;
    }

    // Free mix buffer
    if (ctx->mix_buffer != NULL) {
        free(ctx->mix_buffer);
        ctx->mix_buffer = NULL;
        ctx->mix_buffer_len = 0;
    }

    ctx->is_loaded = false;
    ctx->mute_mask = 0;
}

esp_err_t micromod_backend_start_player(micromod_backend_t *ctx, uint32_t sample_rate, int format) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    // Stop any existing playback
    micromod_backend_end_player(ctx);

    ctx->sample_rate = sample_rate;
    ctx->format = format;

    // Create replay context
    ctx->replay = new_replay(ctx->mod, sample_rate, ctx->interpolation);
    if (ctx->replay == NULL) {
        ESP_LOGE(TAG, "Failed to create replay context");
        return ESP_ERR_NO_MEM;
    }

    // Allocate mix buffer
    // IBXM requires buffer length from calculate_mix_buf_len()
    ctx->mix_buffer_len = calculate_mix_buf_len(sample_rate);
    // Allocate extra space for the mixing overhead IBXM needs
    int alloc_size = (ctx->mix_buffer_len + 65) * 4 * sizeof(int);
    ctx->mix_buffer = (int *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->mix_buffer == NULL) {
        ctx->mix_buffer = (int *)malloc(alloc_size);
        if (ctx->mix_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mix buffer");
            dispose_replay(ctx->replay);
            ctx->replay = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    memset(ctx->mix_buffer, 0, alloc_size);

    ctx->mix_buffer_pos = 0;
    ctx->mix_buffer_avail = 0;
    ctx->song_ended = false;
    ctx->is_playing = true;

    ESP_LOGI(TAG, "Player started at %lu Hz, format: %s, interpolation: %d",
             sample_rate,
             (format == MICROMOD_FORMAT_MONO) ? "MONO" : "STEREO",
             ctx->interpolation);

    return ESP_OK;
}

void micromod_backend_end_player(micromod_backend_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    ctx->is_playing = false;
    ctx->mix_buffer_pos = 0;
    ctx->mix_buffer_avail = 0;
}

int micromod_backend_play_buffer(micromod_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop) {
    if (ctx == NULL || ctx->replay == NULL || !ctx->is_playing) {
        return -1;
    }

    // Calculate number of samples requested (buffer_size is in bytes)
    size_t num_samples = buffer_size / sizeof(int16_t);
    size_t samples_written = 0;

    while (samples_written < num_samples) {
        // If we have samples in the mix buffer, use them
        if (ctx->mix_buffer_avail > 0) {
            // Convert from stereo int32 to mono/stereo int16
            while (samples_written < num_samples && ctx->mix_buffer_avail > 0) {
                int left = ctx->mix_buffer[ctx->mix_buffer_pos * 2];
                int right = ctx->mix_buffer[ctx->mix_buffer_pos * 2 + 1];

                // Clamp to int16 range
                if (left < -32768) left = -32768;
                if (left > 32767) left = 32767;
                if (right < -32768) right = -32768;
                if (right > 32767) right = 32767;

                if (ctx->format == MICROMOD_FORMAT_MONO) {
                    // Mix to mono
                    int mono = (left + right) / 2;
                    buffer[samples_written++] = (int16_t)mono;
                } else {
                    // Output stereo (interleaved L/R)
                    buffer[samples_written++] = (int16_t)left;
                    if (samples_written < num_samples) {
                        buffer[samples_written++] = (int16_t)right;
                    }
                }

                ctx->mix_buffer_pos++;
                ctx->mix_buffer_avail--;
            }
        } else {
            // Need to render more audio
            if (ctx->song_ended) {
                if (loop) {
                    // Restart from beginning
                    replay_set_sequence_pos(ctx->replay, 0);
                    ctx->song_ended = false;
                } else {
                    // Fill remaining with silence and return end
                    memset(&buffer[samples_written], 0,
                           (num_samples - samples_written) * sizeof(int16_t));
                    return -1;
                }
            }

            // Render next tick of audio
            int rendered = replay_get_audio(ctx->replay, ctx->mix_buffer, ctx->mute_mask);
            if (rendered <= 0) {
                // Song ended
                ctx->song_ended = true;
                continue;  // Will handle loop/end above
            }

            ctx->mix_buffer_pos = 0;
            ctx->mix_buffer_avail = rendered;
        }
    }

    return 0;
}

esp_err_t micromod_backend_get_frame_info(micromod_backend_t *ctx, micromod_frame_info_t *frame_info) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || frame_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Get position info from replay (if playing) or default
    if (ctx->replay != NULL) {
        frame_info->pos = replay_get_sequence_pos(ctx->replay);
        frame_info->row = replay_get_row(ctx->replay);
        frame_info->speed = replay_get_speed(ctx->replay);
        frame_info->bpm = replay_get_tempo(ctx->replay);
    } else {
        frame_info->pos = 0;
        frame_info->row = 0;
        frame_info->speed = ctx->mod->default_speed;
        frame_info->bpm = ctx->mod->default_tempo;
    }

    // Get pattern from sequence
    if (frame_info->pos >= 0 && frame_info->pos < ctx->mod->sequence_len) {
        frame_info->pattern = ctx->mod->sequence[frame_info->pos];
    } else {
        frame_info->pattern = 0;
    }

    // Calculate frame time
    frame_info->frame_time = (1000000.0f / ctx->sample_rate);

    // Number of channels
    frame_info->num_channels = ctx->mod->num_channels;
    if (frame_info->num_channels > MICROMOD_MAX_CHANNELS) {
        frame_info->num_channels = MICROMOD_MAX_CHANNELS;
    }

    // Get channel info for current row
    if (frame_info->pattern < ctx->mod->num_patterns) {
        struct pattern *pat = &ctx->mod->patterns[frame_info->pattern];
        for (int ch = 0; ch < frame_info->num_channels; ch++) {
            unsigned char key, ins, vol, fxt, fxp;
            if (pattern_get_note_data(pat, frame_info->row, ch, &key, &ins, &vol, &fxt, &fxp) == 0) {
                frame_info->channel_info[ch].event.note = key;
                frame_info->channel_info[ch].event.ins = ins;
                // IBXM uses separate volume column and effect, map to fxt/fxp
                frame_info->channel_info[ch].event.fxt = fxt;
                frame_info->channel_info[ch].event.fxp = fxp;
            } else {
                frame_info->channel_info[ch].event.note = 0;
                frame_info->channel_info[ch].event.ins = 0;
                frame_info->channel_info[ch].event.fxt = 0;
                frame_info->channel_info[ch].event.fxp = 0;
            }
        }
    }

    // Zero out remaining channels
    for (int ch = frame_info->num_channels; ch < MICROMOD_MAX_CHANNELS; ch++) {
        frame_info->channel_info[ch].event.note = 0;
        frame_info->channel_info[ch].event.ins = 0;
        frame_info->channel_info[ch].event.fxt = 0;
        frame_info->channel_info[ch].event.fxp = 0;
    }

    return ESP_OK;
}

esp_err_t micromod_backend_get_module_info(micromod_backend_t *ctx, micromod_module_info_t *mod_info) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || mod_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    mod_info->name = ctx->mod->name;
    mod_info->chn = ctx->mod->num_channels;
    mod_info->pat = ctx->mod->num_patterns;

    return ESP_OK;
}

esp_err_t micromod_backend_set_player(micromod_backend_t *ctx, int param, int value) {
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (param) {
        case MICROMOD_PLAYER_INTERP:
            // Map to IBXM interpolation: 0=none, 1=linear, 2=sinc
            switch (value) {
                case MICROMOD_INTERP_NEAREST:
                    ctx->interpolation = 0;
                    break;
                case MICROMOD_INTERP_LINEAR:
                // MICROMOD_INTERP_CUBIC maps to same value (1), handled here
                    ctx->interpolation = 1;
                    break;
                case MICROMOD_INTERP_SINC:
                    ctx->interpolation = 2;
                    break;
                default:
                    ctx->interpolation = 1;
                    break;
            }
            // Note: Changing interpolation requires restarting the player
            // to take effect (replay is created with interpolation setting)
            break;

        case MICROMOD_PLAYER_DSP:
            // Not supported by IBXM
            break;

        case MICROMOD_PLAYER_VOICES:
            // Not applicable to IBXM
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

int micromod_backend_channel_mute(micromod_backend_t *ctx, int channel, int status) {
    if (ctx == NULL || ctx->mod == NULL) {
        return -1;
    }

    int num_channels = ctx->mod->num_channels;
    if (channel < 0 || channel >= num_channels) {
        return -1;
    }

    uint32_t channel_bit = (1U << channel);

    // Query current state
    if (status == -1) {
        return (ctx->mute_mask & channel_bit) ? 1 : 0;
    }

    // Toggle
    if (status == 2) {
        ctx->mute_mask ^= channel_bit;
        return (ctx->mute_mask & channel_bit) ? 1 : 0;
    }

    // Set mute state
    if (status == 1) {
        ctx->mute_mask |= channel_bit;
    } else {
        ctx->mute_mask &= ~channel_bit;
    }

    return (ctx->mute_mask & channel_bit) ? 1 : 0;
}

esp_err_t micromod_backend_get_pattern_row_channel(micromod_backend_t *ctx,
                                                    int pattern, int row, int channel,
                                                    uint8_t *note, uint8_t *ins,
                                                    uint8_t *fxt, uint8_t *fxp) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pattern < 0 || pattern >= ctx->mod->num_patterns) {
        return ESP_ERR_INVALID_ARG;
    }

    struct pattern *pat = &ctx->mod->patterns[pattern];

    if (row < 0 || row >= pat->num_rows) {
        return ESP_ERR_INVALID_ARG;
    }

    if (channel < 0 || channel >= pat->num_channels) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char key, instrument, vol, effect, param;
    if (pattern_get_note_data(pat, row, channel, &key, &instrument, &vol, &effect, &param) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (note) *note = key;
    if (ins) *ins = instrument;
    if (fxt) *fxt = effect;
    if (fxp) *fxp = param;

    return ESP_OK;
}

esp_err_t micromod_backend_get_pattern_num_rows(micromod_backend_t *ctx, int pattern, int *num_rows) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || num_rows == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pattern < 0 || pattern >= ctx->mod->num_patterns) {
        return ESP_ERR_INVALID_ARG;
    }

    *num_rows = ctx->mod->patterns[pattern].num_rows;
    return ESP_OK;
}

esp_err_t micromod_backend_get_order_pattern(micromod_backend_t *ctx, int order, int *pattern) {
    if (ctx == NULL || ctx->mod == NULL || !ctx->is_loaded || pattern == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (order < 0 || order >= ctx->mod->sequence_len) {
        return ESP_ERR_INVALID_ARG;
    }

    *pattern = ctx->mod->sequence[order];
    return ESP_OK;
}

bool micromod_backend_is_loaded(micromod_backend_t *ctx) {
    return (ctx != NULL && ctx->is_loaded);
}

bool micromod_backend_is_playing(micromod_backend_t *ctx) {
    return (ctx != NULL && ctx->is_playing && ctx->is_loaded);
}

#else // MOD_BACKEND_MICROMOD not defined - provide stub implementations

micromod_backend_t* micromod_backend_create(void) { return NULL; }
void micromod_backend_free(micromod_backend_t *ctx) { (void)ctx; }
esp_err_t micromod_backend_load_module(micromod_backend_t *ctx, const uint8_t *mod_data, size_t mod_size) { (void)ctx; (void)mod_data; (void)mod_size; return ESP_ERR_NOT_SUPPORTED; }
void micromod_backend_release_module(micromod_backend_t *ctx) { (void)ctx; }
esp_err_t micromod_backend_start_player(micromod_backend_t *ctx, uint32_t sample_rate, int format) { (void)ctx; (void)sample_rate; (void)format; return ESP_ERR_NOT_SUPPORTED; }
void micromod_backend_end_player(micromod_backend_t *ctx) { (void)ctx; }
int micromod_backend_play_buffer(micromod_backend_t *ctx, int16_t *buffer, size_t buffer_size, int loop) { (void)ctx; (void)buffer; (void)buffer_size; (void)loop; return -1; }
esp_err_t micromod_backend_get_frame_info(micromod_backend_t *ctx, micromod_frame_info_t *frame_info) { (void)ctx; (void)frame_info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t micromod_backend_get_module_info(micromod_backend_t *ctx, micromod_module_info_t *mod_info) { (void)ctx; (void)mod_info; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t micromod_backend_set_player(micromod_backend_t *ctx, int param, int value) { (void)ctx; (void)param; (void)value; return ESP_ERR_NOT_SUPPORTED; }
int micromod_backend_channel_mute(micromod_backend_t *ctx, int channel, int status) { (void)ctx; (void)channel; (void)status; return -1; }
esp_err_t micromod_backend_get_pattern_row_channel(micromod_backend_t *ctx, int pattern, int row, int channel, uint8_t *note, uint8_t *ins, uint8_t *fxt, uint8_t *fxp) { (void)ctx; (void)pattern; (void)row; (void)channel; (void)note; (void)ins; (void)fxt; (void)fxp; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t micromod_backend_get_pattern_num_rows(micromod_backend_t *ctx, int pattern, int *num_rows) { (void)ctx; (void)pattern; (void)num_rows; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t micromod_backend_get_order_pattern(micromod_backend_t *ctx, int order, int *pattern) { (void)ctx; (void)order; (void)pattern; return ESP_ERR_NOT_SUPPORTED; }
bool micromod_backend_is_loaded(micromod_backend_t *ctx) { (void)ctx; return false; }
bool micromod_backend_is_playing(micromod_backend_t *ctx) { (void)ctx; return false; }

#endif // MOD_BACKEND_MICROMOD
