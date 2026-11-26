// Tunes storage, mounted read-only for the player at BSP_TUNES_PATH. Two places, tried in this order:
//  1. the micro-SD card, if it answers and holds /INDEX.BIN (written by ./sync-tunes as a whole FAT volume)
//  2. the `tunes` flash partition (firmware/s3/partitions.csv): a raw FAT image written by ./sync-tunes --flash
// ./sync-tunes clears the card's first sector before it writes and writes it last, so a card from an interrupted sync
// does not mount and the player falls back to flash.
#include <sys/stat.h>

#include "bsp/bsp.h"
#include "bsp_sd_priv.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bsp_storage";

#define TUNES_PARTITION "tunes"

static bsp_storage_source_t s_source = BSP_STORAGE_NONE;
static uint32_t s_card_sectors;

static bool mount_sd(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    bsp_sd_config(&host, &slot);
    const esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed = false, .max_files = 4};
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_TUNES_PATH, &host, &slot, &cfg, &card);
    if (card) {
        s_card_sectors = (uint32_t)card->csd.capacity;
    }
    if (err != ESP_OK) {
        // A failed mount frees the card handle and the host, so ask the card itself whether it is there: a card with
        // no volume (new, or from an interrupted sync) must still be reported, or ./sync-tunes could not write it.
        if (!card && bsp_sd_init() == ESP_OK) {
            s_card_sectors = bsp_sd_sectors();
        }
        ESP_LOGI(TAG, "SD card: %s (%s)", s_card_sectors ? "no tunes volume" : "none", esp_err_to_name(err));
        return false;
    }
    struct stat st;
    if (stat(BSP_TUNES_PATH "/INDEX.BIN", &st) != 0) {
        ESP_LOGW(TAG, "SD card has no INDEX.BIN: not a tunes card (run ./sync-tunes)");
        esp_vfs_fat_sdcard_unmount(BSP_TUNES_PATH, card);
        return false;
    }
    ESP_LOGI(TAG, "SD card %s (%u MB) mounted at %s", card->cid.name,
             (unsigned)((uint64_t)card->csd.capacity * 512 / (1024 * 1024)), BSP_TUNES_PATH);
    return true;
}

esp_err_t bsp_storage_mount(void)
{
    if (mount_sd()) {
        s_source = BSP_STORAGE_SD;
        return ESP_OK;
    }
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_fat_spiflash_mount_ro(BSP_TUNES_PATH, TUNES_PARTITION, &cfg), TAG,
                        "mount partition \"%s\" at %s (run ./sync-tunes?)", TUNES_PARTITION, BSP_TUNES_PATH);
    ESP_LOGI(TAG, "partition \"%s\" mounted read-only at %s", TUNES_PARTITION, BSP_TUNES_PATH);
    s_source = BSP_STORAGE_FLASH;
    return ESP_OK;
}

bsp_storage_source_t bsp_storage_source(void)
{
    return s_source;
}

uint32_t bsp_storage_card_sectors(void)
{
    return s_card_sectors;
}
