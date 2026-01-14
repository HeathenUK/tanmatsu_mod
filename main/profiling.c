#include "profiling.h"

#if ENABLE_PROFILING

#include <string.h>

static const char *TAG = "profiling";

profiling_stats_t g_profiling_stats = {0};

void profiling_init(void) {
    memset(&g_profiling_stats, 0, sizeof(g_profiling_stats));
    g_profiling_stats.min_frame_time_us = UINT32_MAX;
    g_profiling_stats.last_report_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "Profiling enabled");
}

void profiling_report(void) {
    TickType_t current_tick = xTaskGetTickCount();
    TickType_t elapsed_ticks = current_tick - g_profiling_stats.last_report_tick;
    uint32_t elapsed_ms = (elapsed_ticks * 1000) / configTICK_RATE_HZ;
    
    if (elapsed_ms >= PROFILING_REPORT_INTERVAL_MS) {
        if (g_profiling_stats.render_count > 0) {
            uint32_t avg_render_us = g_profiling_stats.total_render_time_us / g_profiling_stats.render_count;
            uint32_t avg_blit_us = 0;
            uint32_t total_render_cycle_us = avg_render_us;
            
            if (g_profiling_stats.blit_count > 0) {
                avg_blit_us = g_profiling_stats.total_blit_time_us / g_profiling_stats.blit_count;
                total_render_cycle_us = avg_render_us + avg_blit_us;
            }
            
            ESP_LOGI(TAG, "=== Performance Report ===");
            ESP_LOGI(TAG, "Renders: %lu in %lu ms", g_profiling_stats.render_count, elapsed_ms);
            ESP_LOGI(TAG, "Render time: Avg: %lu us (%.2f ms), Max: %lu us (%.2f ms)",
                    avg_render_us, avg_render_us / 1000.0f,
                    g_profiling_stats.max_render_time_us, g_profiling_stats.max_render_time_us / 1000.0f);
            
            if (g_profiling_stats.blit_count > 0) {
                ESP_LOGI(TAG, "Blit time: Avg: %lu us (%.2f ms), Max: %lu us (%.2f ms)",
                        avg_blit_us, avg_blit_us / 1000.0f,
                        g_profiling_stats.max_blit_time_us, g_profiling_stats.max_blit_time_us / 1000.0f);
                ESP_LOGI(TAG, "Total cycle: Avg: %lu us (%.2f ms)",
                        total_render_cycle_us, total_render_cycle_us / 1000.0f);
            }
            
            if (g_profiling_stats.audio_buffer_count > 0) {
                uint32_t avg_audio_us = g_profiling_stats.total_audio_time_us / g_profiling_stats.audio_buffer_count;
                ESP_LOGI(TAG, "Audio: %lu buffers, Avg: %lu us (%.2f ms), Max: %lu us (%.2f ms)",
                        g_profiling_stats.audio_buffer_count, avg_audio_us, avg_audio_us / 1000.0f,
                        g_profiling_stats.max_audio_time_us, g_profiling_stats.max_audio_time_us / 1000.0f);
            }
            
            ESP_LOGI(TAG, "========================");
        }
        
        // Reset stats for next interval
        memset(&g_profiling_stats, 0, sizeof(g_profiling_stats));
        g_profiling_stats.min_frame_time_us = UINT32_MAX;
        g_profiling_stats.last_report_tick = current_tick;
    }
}

#endif // ENABLE_PROFILING
