// The `tunes` partition (firmware/s3/partitions.csv): a raw FAT image written by ./sync-tunes, mounted read-only.
// Same approach as IDF's examples/storage/fatfs/fatfsgen in read-only mode.
#include "bsp/bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "bsp_storage";

#define TUNES_PARTITION "tunes"

esp_err_t bsp_storage_mount(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_fat_spiflash_mount_ro(BSP_TUNES_PATH, TUNES_PARTITION, &cfg), TAG,
                        "mount partition \"%s\" at %s (run ./sync-tunes?)", TUNES_PARTITION, BSP_TUNES_PATH);
    ESP_LOGI(TAG, "partition \"%s\" mounted read-only at %s", TUNES_PARTITION, BSP_TUNES_PATH);
    return ESP_OK;
}
