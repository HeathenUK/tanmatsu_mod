#include "sdcard.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "hal/ldo_types.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_types.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// BSP SD card pin definitions (matching tanmatsu-launcher)
// These should match the BSP definitions if available
#ifndef BSP_SDCARD_CLK
#define BSP_SDCARD_CLK  GPIO_NUM_43  // CLK on GPIO 43 (matches launcher)
#endif
#ifndef BSP_SDCARD_CMD
#define BSP_SDCARD_CMD  GPIO_NUM_44  // CMD on GPIO 44 (matches launcher)
#endif
#ifndef BSP_SDCARD_D0
#define BSP_SDCARD_D0   GPIO_NUM_39  // D0 on GPIO 39 (matches launcher)
#endif
#ifndef BSP_SDCARD_D1
#define BSP_SDCARD_D1   GPIO_NUM_40  // D1 on GPIO 40 (matches launcher)
#endif
#ifndef BSP_SDCARD_D2
#define BSP_SDCARD_D2   GPIO_NUM_41  // D2 on GPIO 41 (matches launcher)
#endif
#ifndef BSP_SDCARD_D3
#define BSP_SDCARD_D3   GPIO_NUM_42  // D3 on GPIO 42 (matches launcher)
#endif

static const char *TAG = "sdcard";
static bool sdcard_mounted = false;
static sdmmc_card_t *mount_card = NULL;

esp_err_t sdcard_init(void) {
    if (sdcard_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    esp_err_t ret;

    // Configure power control for SD card (LDO4)
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = LDO_UNIT_4, // SDCard powered by VO4
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LDO power control: %s", esp_err_to_name(ret));
        return ret;
    }

    // Power cycle the SD card to ensure it's in a known state
    // This prevents issues when the card was left in SDMMC mode from a previous session
    ESP_LOGI(TAG, "Power cycling SD card...");
    sd_pwr_ctrl_set_io_voltage(pwr_ctrl_handle, 0);      // Power off
    vTaskDelay(pdMS_TO_TICKS(150));                      // Wait 150ms
    sd_pwr_ctrl_set_io_voltage(pwr_ctrl_handle, 3300);   // Power on at 3.3V
    vTaskDelay(pdMS_TO_TICKS(150));                      // Wait 150ms for card to stabilize
    ESP_LOGI(TAG, "SD card power cycle complete");

    // Configure SDMMC slot 0 (matching tanmatsu-launcher - GPIO 43/44 are dedicated IOs for slot 0)
    ESP_LOGI(TAG, "Configuring SDMMC slot 0");
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk    = BSP_SDCARD_CLK;  // GPIO 43 (dedicated IO for slot 0)
    slot_config.cmd    = BSP_SDCARD_CMD;  // GPIO 44 (dedicated IO for slot 0)
    slot_config.d0     = BSP_SDCARD_D0;   // GPIO 39
    slot_config.d1     = BSP_SDCARD_D1;   // GPIO 40
    slot_config.d2     = BSP_SDCARD_D2;   // GPIO 41
    slot_config.d3     = BSP_SDCARD_D3;   // GPIO 42
    slot_config.width  = 4;
    // Slot 0 uses dedicated IOs (GPIO 43/44), no need for internal pullup flag

    // Configure SDMMC host for slot 0 (matching tanmatsu-launcher)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;  // Use SLOT0 for native IOMUX pins (like launcher)
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40MHz (like launcher)
    
    // Allocate DMA buffer in internal RAM to avoid PSRAM cache sync overhead (like launcher)
    static DRAM_DMA_ALIGNED_ATTR uint8_t dma_buf[512 * 4];  // 2KB aligned buffer
    host.dma_aligned_buffer = dma_buf;

    // Configure FAT filesystem mount
    ESP_LOGI(TAG, "Mounting SD card to %s", SD_CARD_MOUNT_POINT);
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed   = false,
        .max_files                = 5,
        .allocation_unit_size     = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat              = false,
    };

    ret = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT,
                                   &host,
                                   &slot_config,
                                   &mount_config,
                                   &mount_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    // Print card info
    sdmmc_card_print_info(stdout, mount_card);

    sdcard_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully");
    return ESP_OK;
}

bool sdcard_is_mounted(void) {
    return sdcard_mounted;
}
