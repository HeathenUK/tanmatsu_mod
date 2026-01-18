#include "fkey_icons.h"
#include "stb_image.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "fkey_icons";

// Number of F-keys we support (F1-F6)
#define NUM_FKEYS 6

// Icon storage structure
typedef struct {
    uint16_t *pixels;  // RGB565 pixel data (allocated in PSRAM)
    int width;
    int height;
    bool loaded;
} fkey_icon_t;

static fkey_icon_t fkey_icons[NUM_FKEYS] = {0};

// Convert RGBA8888 to RGB565
static inline uint16_t rgba_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Buffered file data for decoding (file read happens on internal stack, decode on PSRAM stack)
typedef struct {
    uint8_t *data;
    size_t size;
} file_buffer_t;

static file_buffer_t file_buffers[NUM_FKEYS] = {0};

// Decode PNG from pre-loaded buffer (called from task with PSRAM stack)
static void decode_icon(int index) {
    if (!file_buffers[index].data || file_buffers[index].size == 0) {
        return;
    }

    // Decode PNG using stb_image
    int width, height, channels;
    uint8_t *rgba_data = stbi_load_from_memory(
        file_buffers[index].data, file_buffers[index].size,
        &width, &height, &channels, 4);

    // Free the file buffer now that we've decoded
    heap_caps_free(file_buffers[index].data);
    file_buffers[index].data = NULL;
    file_buffers[index].size = 0;

    if (!rgba_data) {
        ESP_LOGW(TAG, "Failed to decode PNG for F%d", index + 1);
        return;
    }

    // Allocate RGB565 buffer in PSRAM
    size_t rgb565_size = width * height * sizeof(uint16_t);
    uint16_t *rgb565_data = (uint16_t *)heap_caps_malloc(rgb565_size, MALLOC_CAP_SPIRAM);
    if (!rgb565_data) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM for F%d icon", index + 1);
        stbi_image_free(rgba_data);
        return;
    }

    // Convert RGBA to RGB565
    for (int p = 0; p < width * height; p++) {
        uint8_t r = rgba_data[p * 4 + 0];
        uint8_t g = rgba_data[p * 4 + 1];
        uint8_t b = rgba_data[p * 4 + 2];
        uint8_t a = rgba_data[p * 4 + 3];

        // Simple alpha handling: blend with black background
        if (a < 255) {
            r = (r * a) / 255;
            g = (g * a) / 255;
            b = (b * a) / 255;
        }

        rgb565_data[p] = rgba_to_rgb565(r, g, b);
    }

    stbi_image_free(rgba_data);

    // Store icon
    fkey_icons[index].pixels = rgb565_data;
    fkey_icons[index].width = width;
    fkey_icons[index].height = height;
    fkey_icons[index].loaded = true;

    ESP_LOGI(TAG, "Loaded F%d icon: %dx%d", index + 1, width, height);
}

// Task that decodes all pre-loaded PNG buffers (runs with PSRAM stack)
static void fkey_decode_task(void *arg) {
    SemaphoreHandle_t done_sem = (SemaphoreHandle_t)arg;

    for (int i = 0; i < NUM_FKEYS; i++) {
        decode_icon(i);
    }

    xSemaphoreGive(done_sem);
    vTaskDelete(NULL);
}

// Stack size for decode task (stb_image needs significant stack)
#define FKEY_DECODE_TASK_STACK_SIZE (32 * 1024)

void fkey_icons_init(void) {
    // Phase 1: Read all files into PSRAM buffers (runs on current task with internal stack)
    char path[64];
    int files_loaded = 0;

    for (int i = 0; i < NUM_FKEYS; i++) {
        snprintf(path, sizeof(path), "/int/icons/keyboard/f%d.png", i + 1);

        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGD(TAG, "Icon not found: %s", path);
            continue;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_size <= 0 || file_size > 64 * 1024) {
            ESP_LOGW(TAG, "Icon file invalid size: %s (%ld bytes)", path, file_size);
            fclose(f);
            continue;
        }

        // Allocate in PSRAM
        uint8_t *file_data = (uint8_t *)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
        if (!file_data) {
            ESP_LOGE(TAG, "Failed to allocate buffer for: %s", path);
            fclose(f);
            continue;
        }

        size_t bytes_read = fread(file_data, 1, file_size, f);
        fclose(f);

        if (bytes_read != (size_t)file_size) {
            ESP_LOGW(TAG, "Failed to read: %s", path);
            heap_caps_free(file_data);
            continue;
        }

        file_buffers[i].data = file_data;
        file_buffers[i].size = file_size;
        files_loaded++;
        ESP_LOGD(TAG, "Buffered %s (%ld bytes)", path, file_size);
    }

    if (files_loaded == 0) {
        ESP_LOGI(TAG, "No icon files found");
        return;
    }

    // Phase 2: Decode PNGs in a task with PSRAM stack
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    StackType_t *task_stack = (StackType_t *)heap_caps_malloc(
        FKEY_DECODE_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM);
    if (!task_stack) {
        ESP_LOGE(TAG, "Failed to allocate decode task stack");
        vSemaphoreDelete(done_sem);
        return;
    }

    StaticTask_t *task_tcb = (StaticTask_t *)heap_caps_malloc(
        sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (!task_tcb) {
        ESP_LOGE(TAG, "Failed to allocate task TCB");
        heap_caps_free(task_stack);
        vSemaphoreDelete(done_sem);
        return;
    }

    TaskHandle_t task_handle = xTaskCreateStatic(
        fkey_decode_task,
        "fkey_dec",
        FKEY_DECODE_TASK_STACK_SIZE / sizeof(StackType_t),
        done_sem,
        tskIDLE_PRIORITY + 1,
        task_stack,
        task_tcb
    );

    if (!task_handle) {
        ESP_LOGE(TAG, "Failed to create decode task");
        heap_caps_free(task_stack);
        heap_caps_free(task_tcb);
        vSemaphoreDelete(done_sem);
        return;
    }

    xSemaphoreTake(done_sem, portMAX_DELAY);

    vSemaphoreDelete(done_sem);
    heap_caps_free(task_stack);
    heap_caps_free(task_tcb);

    ESP_LOGI(TAG, "Icon loading complete");
}

void fkey_icons_free(void) {
    for (int i = 0; i < NUM_FKEYS; i++) {
        if (fkey_icons[i].pixels) {
            heap_caps_free(fkey_icons[i].pixels);
            fkey_icons[i].pixels = NULL;
        }
        fkey_icons[i].loaded = false;
    }
}

bool fkey_icon_available(int fkey_num) {
    if (fkey_num < 1 || fkey_num > NUM_FKEYS) {
        return false;
    }
    return fkey_icons[fkey_num - 1].loaded;
}

bool fkey_icon_get_size(int fkey_num, int *width, int *height) {
    if (fkey_num < 1 || fkey_num > NUM_FKEYS) {
        return false;
    }
    fkey_icon_t *icon = &fkey_icons[fkey_num - 1];
    if (!icon->loaded) {
        return false;
    }
    if (width) *width = icon->width;
    if (height) *height = icon->height;
    return true;
}

void fkey_icon_draw(uint16_t *fb, int fb_width, int fb_height,
                    int x, int y, int fkey_num, int scale) {
    if (!fb || fkey_num < 1 || fkey_num > NUM_FKEYS) {
        return;
    }

    fkey_icon_t *icon = &fkey_icons[fkey_num - 1];
    if (!icon->loaded || !icon->pixels) {
        return;
    }

    if (scale < 1) scale = 1;

    int src_width = icon->width;
    int src_height = icon->height;
    uint16_t *src_pixels = icon->pixels;

    // Draw with scaling
    for (int sy = 0; sy < src_height; sy++) {
        for (int sx = 0; sx < src_width; sx++) {
            uint16_t pixel = src_pixels[sy * src_width + sx];

            // Skip black pixels (treat as transparent)
            if (pixel == 0x0000) {
                continue;
            }

            // Draw scaled pixel block
            for (int dy = 0; dy < scale; dy++) {
                int fy = y + sy * scale + dy;
                if (fy < 0 || fy >= fb_height) continue;

                for (int dx = 0; dx < scale; dx++) {
                    int fx = x + sx * scale + dx;
                    if (fx < 0 || fx >= fb_width) continue;

                    fb[fy * fb_width + fx] = pixel;
                }
            }
        }
    }
}
