/**
 * @file vgm_backend.cpp
 * @brief VGM/VGZ playback backend implementation using libvgm
 */

#include "vgm_backend.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cstdlib>
#include <new>

// libvgm includes
#include "playera.hpp"
#include "vgmplayer.hpp"
#include "DataLoader.h"
#include "MemoryLoader.h"
#include "SoundEmu.h"
#include "EmuCores.h"

// For VGZ decompression - use miniz from esp_rom
extern "C" {
#include "miniz.h"
}

static const char *TAG = "vgm_backend";

static bool vgm_device_has_core(const DEV_DECL *decl) {
    return decl && decl->cores[0];
}

// VGM backend context structure
struct vgm_backend {
    PlayerA *player;
    VGMPlayer *vgmPlayer;
    DATA_LOADER *dataLoader;
    uint8_t *vgmData;           // Decompressed VGM data
    size_t vgmDataSize;
    uint32_t sampleRate;
    bool loaded;
    bool playing;
    uint32_t loopCount;
    uint32_t fadeSamples;
};

// Check if data is gzip compressed (VGZ)
static bool is_gzip(const uint8_t *data, size_t size) {
    if (size < 2) return false;
    return (data[0] == 0x1F && data[1] == 0x8B);
}

// Decompress VGZ to VGM using tinfl with proper loop-based decompression
static uint8_t *decompress_vgz(const uint8_t *data, size_t size, size_t *out_size) {
    if (size < 18) return NULL;  // Minimum gzip file size

    // Parse gzip header
    size_t header_size = 10;  // Basic gzip header
    uint8_t flags = data[3];

    // Skip optional fields
    if (flags & 0x04) {  // FEXTRA
        if (header_size + 2 > size) return NULL;
        uint16_t xlen = data[header_size] | (data[header_size + 1] << 8);
        header_size += 2 + xlen;
    }
    if (flags & 0x08) {  // FNAME
        while (header_size < size && data[header_size] != 0) header_size++;
        header_size++;
    }
    if (flags & 0x10) {  // FCOMMENT
        while (header_size < size && data[header_size] != 0) header_size++;
        header_size++;
    }
    if (flags & 0x02) {  // FHCRC
        header_size += 2;
    }

    if (header_size >= size - 8) return NULL;

    // Get original size from gzip trailer (last 4 bytes, little-endian)
    size_t orig_size = data[size - 4] | (data[size - 3] << 8) |
                       (data[size - 2] << 16) | (data[size - 1] << 24);

    if (orig_size > 32 * 1024 * 1024) {
        ESP_LOGE(TAG, "VGZ decompressed size too large: %zu", orig_size);
        return NULL;
    }

    ESP_LOGI(TAG, "VGZ: compressed=%zu, header=%zu, expected=%zu", size, header_size, orig_size);

    // Allocate output buffer in PSRAM
    uint8_t *out = (uint8_t *)heap_caps_malloc(orig_size, MALLOC_CAP_SPIRAM);
    if (!out) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for VGZ decompression", orig_size);
        return NULL;
    }

    // Allocate decompressor on heap (~11KB)
    tinfl_decompressor *decomp = (tinfl_decompressor *)heap_caps_malloc(
        sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    if (!decomp) {
        ESP_LOGE(TAG, "Failed to allocate decompressor");
        heap_caps_free(out);
        return NULL;
    }
    tinfl_init(decomp);

    // Set up deflate data pointers
    const mz_uint8 *in_ptr = (const mz_uint8 *)(data + header_size);
    size_t in_remaining = size - header_size - 8;  // Exclude header and trailer
    mz_uint8 *out_ptr = (mz_uint8 *)out;
    size_t out_remaining = orig_size;
    size_t total_out = 0;

    // Decompress in a loop
    tinfl_status status;
    do {
        size_t in_bytes = in_remaining;
        size_t out_bytes = out_remaining;

        mz_uint32 flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (in_remaining > 0) {
            flags |= TINFL_FLAG_HAS_MORE_INPUT;
        }

        status = tinfl_decompress(decomp, in_ptr, &in_bytes,
                                  (mz_uint8 *)out, out_ptr, &out_bytes, flags);

        in_ptr += in_bytes;
        in_remaining -= in_bytes;
        out_ptr += out_bytes;
        out_remaining -= out_bytes;
        total_out += out_bytes;

        ESP_LOGD(TAG, "tinfl loop: status=%d, in_consumed=%zu, out_written=%zu, total=%zu",
                 status, in_bytes, out_bytes, total_out);

    } while (status == TINFL_STATUS_HAS_MORE_OUTPUT ||
             (status == TINFL_STATUS_NEEDS_MORE_INPUT && in_remaining > 0));

    heap_caps_free(decomp);

    if (status != TINFL_STATUS_DONE) {
        ESP_LOGE(TAG, "VGZ decompression failed: status=%d, total_out=%zu", status, total_out);
        heap_caps_free(out);
        return NULL;
    }

    *out_size = total_out;
    ESP_LOGI(TAG, "VGZ decompressed: %zu -> %zu bytes", size, *out_size);
    return out;
}

extern "C" {

vgm_backend_t *vgm_backend_create(void) {
    vgm_backend_t *ctx = (vgm_backend_t *)heap_caps_calloc(1, sizeof(vgm_backend_t), MALLOC_CAP_SPIRAM);
    if (!ctx) {
        ESP_LOGE(TAG, "Failed to allocate VGM backend context");
        return NULL;
    }

    // Create PlayerA instance in PSRAM
    void *player_mem = heap_caps_malloc(sizeof(PlayerA), MALLOC_CAP_SPIRAM);
    if (!player_mem) {
        ESP_LOGE(TAG, "Failed to allocate PlayerA in PSRAM");
        heap_caps_free(ctx);
        return NULL;
    }
    ctx->player = new (player_mem) PlayerA();
    if (!ctx->player) {
        ESP_LOGE(TAG, "Failed to create PlayerA");
        heap_caps_free(player_mem);
        heap_caps_free(ctx);
        return NULL;
    }

    // Create VGMPlayer instance and register it
    void *vgmplayer_mem = heap_caps_malloc(sizeof(VGMPlayer), MALLOC_CAP_SPIRAM);
    if (!vgmplayer_mem) {
        ESP_LOGE(TAG, "Failed to allocate VGMPlayer in PSRAM");
        ctx->player->~PlayerA();
        heap_caps_free(ctx->player);
        heap_caps_free(ctx);
        return NULL;
    }
    ctx->vgmPlayer = new (vgmplayer_mem) VGMPlayer();
    if (!ctx->vgmPlayer) {
        ESP_LOGE(TAG, "Failed to create VGMPlayer");
        heap_caps_free(vgmplayer_mem);
        ctx->player->~PlayerA();
        heap_caps_free(ctx->player);
        heap_caps_free(ctx);
        return NULL;
    }

    ctx->player->RegisterPlayerEngine(ctx->vgmPlayer);

    // Default settings
    ctx->loopCount = 2;  // Play twice (1 loop)
    ctx->fadeSamples = 44100 * 3;  // 3 second fade
    ctx->sampleRate = VGM_PLAYBACK_SAMPLE_RATE;

    ESP_LOGI(TAG, "VGM backend created");
    return ctx;
}

void vgm_backend_free(vgm_backend_t *ctx) {
    if (!ctx) return;

    vgm_backend_unload(ctx);

    if (ctx->player) {
        ctx->player->UnregisterAllPlayers();
        ctx->player->~PlayerA();
        heap_caps_free(ctx->player);
    }
    if (ctx->vgmPlayer) {
        ctx->vgmPlayer->~VGMPlayer();
        heap_caps_free(ctx->vgmPlayer);
    }

    heap_caps_free(ctx);
    ESP_LOGI(TAG, "VGM backend freed");
}

esp_err_t vgm_backend_load(vgm_backend_t *ctx, const uint8_t *data, size_t size) {
    if (!ctx || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Unload any existing file
    vgm_backend_unload(ctx);

    const uint8_t *vgm_data = data;
    size_t vgm_size = size;

    // Check for VGZ (gzip compressed)
    if (is_gzip(data, size)) {
        ESP_LOGI(TAG, "Detected VGZ file, decompressing...");
        ctx->vgmData = decompress_vgz(data, size, &ctx->vgmDataSize);
        if (!ctx->vgmData) {
            return ESP_ERR_NO_MEM;
        }
        vgm_data = ctx->vgmData;
        vgm_size = ctx->vgmDataSize;
    }

    // Verify VGM header
    if (vgm_size < 64 || memcmp(vgm_data, "Vgm ", 4) != 0) {
        ESP_LOGE(TAG, "Invalid VGM file header");
        if (ctx->vgmData) {
            heap_caps_free(ctx->vgmData);
            ctx->vgmData = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }

    // Create memory loader
    ctx->dataLoader = MemoryLoader_Init(vgm_data, vgm_size);
    if (!ctx->dataLoader) {
        ESP_LOGE(TAG, "Failed to create data loader");
        if (ctx->vgmData) {
            heap_caps_free(ctx->vgmData);
            ctx->vgmData = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    // Load the file
    UINT8 ret = DataLoader_Load(ctx->dataLoader);
    if (ret != 0) {
        ESP_LOGE(TAG, "DataLoader_Load failed: %d", ret);
        DataLoader_Deinit(ctx->dataLoader);
        ctx->dataLoader = NULL;
        if (ctx->vgmData) {
            heap_caps_free(ctx->vgmData);
            ctx->vgmData = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }

    // Load into player
    ret = ctx->player->LoadFile(ctx->dataLoader);
    if (ret != 0) {
        ESP_LOGE(TAG, "PlayerA::LoadFile failed: %d", ret);
        DataLoader_Deinit(ctx->dataLoader);
        ctx->dataLoader = NULL;
        if (ctx->vgmData) {
            heap_caps_free(ctx->vgmData);
            ctx->vgmData = NULL;
        }
        return ESP_ERR_INVALID_ARG;
    }

    // Reject files that reference devices without a built core
    {
        std::vector<PLR_DEV_INFO> devInfoList;
        bool unsupported = false;
        if (ctx->vgmPlayer->GetSongDeviceInfo(devInfoList) == 0) {
            for (const auto &devInfo : devInfoList) {
                if (!vgm_device_has_core(devInfo.devDecl)) {
                    ESP_LOGE(TAG, "Unsupported VGM device: type=0x%02x (no core built)",
                             devInfo.type);
                    unsupported = true;
                }
                for (const auto &linkDev : devInfo.devLink) {
                    if (!vgm_device_has_core(linkDev.devDecl)) {
                        ESP_LOGE(TAG, "Unsupported linked device: type=0x%02x (no core built)",
                                 linkDev.type);
                        unsupported = true;
                    }
                }
            }
        }
        if (unsupported) {
            ctx->player->UnloadFile();
            DataLoader_Deinit(ctx->dataLoader);
            ctx->dataLoader = NULL;
            if (ctx->vgmData) {
                heap_caps_free(ctx->vgmData);
                ctx->vgmData = NULL;
            }
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    ctx->loaded = true;

    // Get file info
    PLR_SONG_INFO songInfo;
    if (ctx->vgmPlayer->GetSongInfo(songInfo) == 0) {
        ESP_LOGI(TAG, "VGM loaded: %u ticks, %u devices", songInfo.songLen, songInfo.deviceCnt);
    }

    return ESP_OK;
}

esp_err_t vgm_backend_unload(vgm_backend_t *ctx) {
    if (!ctx) return ESP_ERR_INVALID_ARG;

    if (ctx->playing) {
        vgm_backend_stop(ctx);
    }

    if (ctx->loaded) {
        ctx->player->UnloadFile();
        ctx->loaded = false;
    }

    if (ctx->dataLoader) {
        DataLoader_Deinit(ctx->dataLoader);
        ctx->dataLoader = NULL;
    }

    if (ctx->vgmData) {
        heap_caps_free(ctx->vgmData);
        ctx->vgmData = NULL;
        ctx->vgmDataSize = 0;
    }

    return ESP_OK;
}

esp_err_t vgm_backend_start(vgm_backend_t *ctx, uint32_t sample_rate) {
    if (!ctx || !ctx->loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx->sampleRate = sample_rate;

    // Configure output: stereo 16-bit, 4096 sample buffer
    // The 4th parameter is the internal sample buffer length - must be > 0!
    UINT8 ret = ctx->player->SetOutputSettings(sample_rate, 2, 16, 4096);
    if (ret != 0) {
        ESP_LOGE(TAG, "SetOutputSettings failed: %d", ret);
        return ESP_FAIL;
    }

    // Configure playback
    ctx->player->SetLoopCount(ctx->loopCount);
    ctx->player->SetFadeSamples(ctx->fadeSamples);
    ctx->player->SetEndSilenceSamples(sample_rate / 2);  // 0.5s silence at end

    // Start playback
    ret = ctx->player->Start();
    if (ret != 0) {
        ESP_LOGE(TAG, "Start failed: %d", ret);
        return ESP_FAIL;
    }

    ctx->playing = true;
    ESP_LOGI(TAG, "VGM playback started at %u Hz", sample_rate);
    return ESP_OK;
}

esp_err_t vgm_backend_stop(vgm_backend_t *ctx) {
    if (!ctx) return ESP_ERR_INVALID_ARG;

    if (ctx->playing) {
        ctx->player->Stop();
        ctx->playing = false;
        ESP_LOGI(TAG, "VGM playback stopped");
    }

    return ESP_OK;
}

uint32_t vgm_backend_render(vgm_backend_t *ctx, int16_t *buffer, uint32_t num_samples) {
    if (!ctx || !ctx->playing || !buffer) {
        return 0;
    }

    // Check if playback finished
    UINT8 state = ctx->player->GetState();
    if (state & PLAYSTATE_FIN) {
        ctx->playing = false;
        return 0;
    }

    // Render samples (buffer size in bytes)
    // Stereo 16-bit = 4 bytes per sample frame
    UINT32 rendered = ctx->player->Render(num_samples * 4, buffer);

    // Convert from bytes to sample frames
    uint32_t sample_frames = rendered / 4;

    // Attenuate output slightly (-6dB) to reduce clipping risk
    for (uint32_t i = 0; i < sample_frames * 2; i++) {
        int32_t sample = ((int32_t)buffer[i]) >> 1;
        if (sample > 32767) sample = 32767;
        else if (sample < -32768) sample = -32768;
        buffer[i] = (int16_t)sample;
    }

    return sample_frames;
}

esp_err_t vgm_backend_get_tags(vgm_backend_t *ctx, vgm_tags_t *tags) {
    if (!ctx || !tags) return ESP_ERR_INVALID_ARG;
    if (!ctx->loaded) return ESP_ERR_INVALID_STATE;

    memset(tags, 0, sizeof(vgm_tags_t));

    const char* const* tagList = ctx->vgmPlayer->GetTags();
    if (!tagList) return ESP_OK;

    // Parse tag pairs: [name, value, name, value, ..., NULL]
    for (int i = 0; tagList[i] != NULL; i += 2) {
        const char *name = tagList[i];
        const char *value = tagList[i + 1];
        if (!value) break;

        if (strcmp(name, "TITLE") == 0) {
            strncpy(tags->title, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "TITLE-JPN") == 0) {
            strncpy(tags->title_jpn, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "GAME") == 0) {
            strncpy(tags->game, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "GAME-JPN") == 0) {
            strncpy(tags->game_jpn, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "SYSTEM") == 0) {
            strncpy(tags->system, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "SYSTEM-JPN") == 0) {
            strncpy(tags->system_jpn, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "ARTIST") == 0) {
            strncpy(tags->author, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "ARTIST-JPN") == 0) {
            strncpy(tags->author_jpn, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "DATE") == 0) {
            strncpy(tags->date, value, sizeof(tags->date) - 1);
        } else if (strcmp(name, "ENCODED_BY") == 0) {
            strncpy(tags->encoded_by, value, VGM_TAG_MAX_LEN - 1);
        } else if (strcmp(name, "COMMENT") == 0) {
            strncpy(tags->comment, value, VGM_TAG_MAX_LEN - 1);
        }
    }

    return ESP_OK;
}

esp_err_t vgm_backend_get_info(vgm_backend_t *ctx, vgm_playback_info_t *info) {
    if (!ctx || !info) return ESP_ERR_INVALID_ARG;

    memset(info, 0, sizeof(vgm_playback_info_t));
    info->sample_rate = ctx->sampleRate;
    info->is_playing = ctx->playing;

    if (!ctx->loaded) return ESP_OK;

    PLR_SONG_INFO songInfo;
    if (ctx->vgmPlayer->GetSongInfo(songInfo) == 0) {
        info->num_chips = songInfo.deviceCnt;
        info->has_loop = (songInfo.loopTick != (UINT32)-1);

        // Convert ticks to samples using player's conversion
        info->total_samples = ctx->vgmPlayer->Tick2Sample(songInfo.songLen);
        if (info->has_loop) {
            UINT32 loopLen = songInfo.songLen - songInfo.loopTick;
            info->loop_samples = ctx->vgmPlayer->Tick2Sample(loopLen);
        }

        // Calculate times
        info->total_time_sec = ctx->vgmPlayer->Tick2Second(songInfo.songLen);
        if (info->has_loop) {
            info->loop_time_sec = ctx->vgmPlayer->Tick2Second(songInfo.loopTick);
        }
    } else {
        ESP_LOGW(TAG, "GetSongInfo failed; falling back to device list");
    }

    {
        std::vector<PLR_DEV_INFO> devInfoList;
        UINT8 devInfoRet = ctx->vgmPlayer->GetSongDeviceInfo(devInfoList);
        if (devInfoRet == 0x00 || devInfoRet == 0x01) {
            if (!devInfoList.empty()) {
                info->num_chips = (uint8_t)devInfoList.size();
            }
        }
    }

    if (ctx->playing) {
        info->current_sample = ctx->player->GetCurPos(PLAYPOS_SAMPLE);
        info->current_time_sec = ctx->player->GetCurTime(PLAYTIME_LOOP_INCL);
        info->has_ended = (ctx->player->GetState() & PLAYSTATE_FIN) != 0;
    }

    return ESP_OK;
}

esp_err_t vgm_backend_get_chip_info(vgm_backend_t *ctx, uint8_t index, vgm_chip_info_t *chip_info) {
    if (!ctx || !chip_info || !ctx->loaded) return ESP_ERR_INVALID_ARG;

    std::vector<PLR_DEV_INFO> devInfoList;
    UINT8 devInfoRet = ctx->vgmPlayer->GetSongDeviceInfo(devInfoList);
    if (devInfoRet != 0x00 && devInfoRet != 0x01) {
        return ESP_FAIL;
    }

    if (index >= devInfoList.size()) {
        return ESP_ERR_INVALID_ARG;
    }

    const PLR_DEV_INFO &devInfo = devInfoList[index];
    chip_info->type = devInfo.type;

    // Get chip name from device declaration (name is a function that takes devCfg)
    if (devInfo.devDecl && devInfo.devDecl->name && devInfo.devCfg) {
        chip_info->name = devInfo.devDecl->name(devInfo.devCfg);
    } else {
        chip_info->name = "Unknown";
    }

    // Get clock from config
    if (devInfo.devCfg) {
        chip_info->clock = devInfo.devCfg->clock;
    } else {
        chip_info->clock = 0;
    }

    return ESP_OK;
}

bool vgm_backend_is_loaded(vgm_backend_t *ctx) {
    return ctx && ctx->loaded;
}

bool vgm_backend_is_playing(vgm_backend_t *ctx) {
    return ctx && ctx->playing;
}

esp_err_t vgm_backend_seek(vgm_backend_t *ctx, uint32_t sample_pos) {
    if (!ctx || !ctx->loaded) return ESP_ERR_INVALID_STATE;

    UINT8 ret = ctx->player->Seek(PLAYPOS_SAMPLE, sample_pos);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

void vgm_backend_set_loop_count(vgm_backend_t *ctx, uint32_t loops) {
    if (!ctx) return;
    ctx->loopCount = loops;
    if (ctx->playing) {
        ctx->player->SetLoopCount(loops);
    }
}

void vgm_backend_set_fade_time(vgm_backend_t *ctx, uint32_t fade_ms) {
    if (!ctx) return;
    ctx->fadeSamples = (ctx->sampleRate * fade_ms) / 1000;
    if (ctx->playing) {
        ctx->player->SetFadeSamples(ctx->fadeSamples);
    }
}

}  // extern "C"
