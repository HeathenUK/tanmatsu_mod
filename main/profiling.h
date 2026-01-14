#ifndef PROFILING_H
#define PROFILING_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

// Enable profiling by defining ENABLE_PROFILING
#define ENABLE_PROFILING 1

#if ENABLE_PROFILING

// Profiling statistics
typedef struct {
    uint32_t frame_count;
    uint32_t total_frame_time_us;
    uint32_t max_frame_time_us;
    uint32_t min_frame_time_us;
    
    uint32_t blit_count;
    uint32_t total_blit_time_us;
    uint32_t max_blit_time_us;
    
    uint32_t render_count;
    uint32_t total_render_time_us;
    uint32_t max_render_time_us;
    
    uint32_t audio_buffer_count;
    uint32_t total_audio_time_us;
    uint32_t max_audio_time_us;
    
    TickType_t last_report_tick;
} profiling_stats_t;

extern profiling_stats_t g_profiling_stats;

#define PROFILING_START(var) int64_t var##_start = esp_timer_get_time(); (void)0
#define PROFILING_END(var, stats_field) do { \
    int64_t var##_end = esp_timer_get_time(); \
    uint32_t var##_us = (uint32_t)(var##_end - var##_start); \
    g_profiling_stats.stats_field##_count++; \
    g_profiling_stats.total_##stats_field##_time_us += var##_us; \
    if (var##_us > g_profiling_stats.max_##stats_field##_time_us) { \
        g_profiling_stats.max_##stats_field##_time_us = var##_us; \
    } \
} while(0)

#define PROFILING_END_FRAME(var) do { \
    int64_t var##_end = esp_timer_get_time(); \
    uint32_t var##_us = (uint32_t)(var##_end - var##_start); \
    g_profiling_stats.frame_count++; \
    g_profiling_stats.total_frame_time_us += var##_us; \
    if (var##_us > g_profiling_stats.max_frame_time_us) { \
        g_profiling_stats.max_frame_time_us = var##_us; \
    } \
    if (var##_us < g_profiling_stats.min_frame_time_us) { \
        g_profiling_stats.min_frame_time_us = var##_us; \
    } \
} while(0)

#define PROFILING_REPORT_INTERVAL_MS 5000  // Report every 5 seconds

void profiling_init(void);
void profiling_report(void);

#else

#define PROFILING_START(var) (void)0
#define PROFILING_END(var, stats_field) (void)0
#define profiling_init() (void)0
#define profiling_report() (void)0

#endif

#endif // PROFILING_H
