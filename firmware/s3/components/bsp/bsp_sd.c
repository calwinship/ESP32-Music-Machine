// micro-SD card over SDMMC 4-bit (pins: board_pins.h). Two uses, never in the same boot:
//  - player: the card's FAT volume mounted at BSP_TUNES_PATH (bsp_storage.c), read only by the player
//  - sync mode: raw sector writes from ./sync-tunes (main/link_task.c)
#include <string.h>

#include "bsp/bsp.h"
#include "bsp_sd_priv.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bsp_sd";
static sdmmc_card_t s_card;
static bool s_ready;
static uint8_t *s_verify;

void bsp_sd_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->max_freq_khz = SDMMC_FREQ_DEFAULT;   // 20 MHz: plenty for audio, verified in the 2026-11-16 probe
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = 4;
    slot->clk = BSP_SD_PIN_CLK;
    slot->cmd = BSP_SD_PIN_CMD;
    slot->d0 = BSP_SD_PIN_D0;
    slot->d1 = BSP_SD_PIN_D1;
    slot->d2 = BSP_SD_PIN_D2;
    slot->d3 = BSP_SD_PIN_D3;
}

esp_err_t bsp_sd_init(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    bsp_sd_config(&host, &slot);
    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "slot init");
    esp_err_t err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(err));
        sdmmc_host_deinit();
        return err;
    }
    s_ready = true;
    ESP_LOGI(TAG, "card %s: %u sectors (%u MB)", s_card.cid.name, (unsigned)s_card.csd.capacity,
             (unsigned)((uint64_t)s_card.csd.capacity * 512 / (1024 * 1024)));
    return ESP_OK;
}

uint32_t bsp_sd_sectors(void)
{
    return s_ready ? (uint32_t)s_card.csd.capacity : bsp_storage_card_sectors();
}

esp_err_t bsp_sd_write_verified(uint32_t first, const void *data, uint32_t count)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "card not initialised");
    ESP_RETURN_ON_FALSE(count && count <= BSP_SD_MAX_SECTORS, ESP_ERR_INVALID_ARG, TAG, "count %u", (unsigned)count);
    ESP_RETURN_ON_FALSE((uint64_t)first + count <= s_card.csd.capacity, ESP_ERR_INVALID_SIZE, TAG, "past the end");
    if (!s_verify) {   // only sync mode writes, so only it pays for the read-back buffer
        s_verify = heap_caps_malloc(BSP_SD_MAX_SECTORS * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_verify, ESP_ERR_NO_MEM, TAG, "verify buffer");
    }
    ESP_RETURN_ON_ERROR(sdmmc_write_sectors(&s_card, data, first, count), TAG, "write %u+%u", (unsigned)first,
                        (unsigned)count);
    ESP_RETURN_ON_ERROR(sdmmc_read_sectors(&s_card, s_verify, first, count), TAG, "read back");
    return memcmp(s_verify, data, (size_t)count * 512) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}
