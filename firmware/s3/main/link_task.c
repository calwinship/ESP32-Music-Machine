// USB link to ./sync-tunes (frames: logic/link.h). Two modes:
//  - player mode (link_task_start): answers 'I' (what is playing from where) and 'B' (restart into sync mode)
//  - sync mode (link_sync_run, chosen at boot): only the "Syncing tunes" screen and raw SD writes; 'X' restarts
//    into player mode, and so does 60 s without a frame (e.g. the Mac side was stopped)
// Sync mode is requested across the restart by a word in RTC memory, which survives esp_restart but not power-off.
#include <string.h>

#include "bsp/bsp.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_task.h"
#include "logic/link.h"
#include "player_task.h"
#include "ui.h"

static const char *TAG = "link";

#define SYNC_REQUEST   0x53594E43u   // "SYNC"
#define SYNC_IDLE_MS   60000
#define LINK_STACK     4096
#define LINK_PRIO      3

RTC_NOINIT_ATTR static uint32_t s_boot_request;

bool link_sync_requested(void)
{
    bool r = s_boot_request == SYNC_REQUEST;
    s_boot_request = 0;
    return r;
}

static void reply(uint8_t type, const uint32_t *words, size_t count)
{
    uint8_t payload[24], frame[LINK_OVERHEAD + sizeof payload];
    for (size_t i = 0; i < count; i++) {
        link_put_u32(payload + 4 * i, words[i]);
    }
    size_t n = link_encode(type, payload, (uint32_t)(4 * count), frame, sizeof frame);
    if (!bsp_link_write(frame, n)) {
        ESP_LOGW(TAG, "reply '%c' not sent", type);
    }
}

static void restart(void)
{
    bsp_link_flush(500);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

// ---- player mode ---------------------------------------------------------------------------------------------
static void player_link_task(void *arg)
{
    (void)arg;
    static uint8_t rxbuf[64];   // commands only; a large frame does not fit and is skipped
    link_rx_t rx;
    link_rx_init(&rx, rxbuf, sizeof rxbuf);
    uint8_t in[128];
    for (;;) {
        int n = bsp_link_read(in, sizeof in, 1000);
        size_t off = 0;
        while (n > 0 && off < (size_t)n) {
            size_t used;
            link_frame_t f;
            bool got = link_rx_feed(&rx, in + off, (size_t)n - off, &used, &f);
            off += used;
            if (!got) {
                continue;
            }
            if (f.type == 'I') {
                player_status_t st;
                player_status_get(&st);
                uint32_t w[5] = {LINK_VERSION, LINK_MODE_PLAYER, (uint32_t)bsp_storage_source(), bsp_sd_sectors(),
                                 st.tune_count};
                reply('i', w, 5);
            } else if (f.type == 'B') {
                ESP_LOGI(TAG, "sync requested: restarting into sync mode");
                reply('b', NULL, 0);
                s_boot_request = SYNC_REQUEST;
                restart();
            }
        }
    }
}

void link_task_start(void)
{
    if (bsp_link_start() != ESP_OK) {
        ESP_LOGE(TAG, "USB link not started: ./sync-tunes cannot reach the board");
        return;
    }
    xTaskCreate(player_link_task, "link", LINK_STACK, NULL, LINK_PRIO, NULL);
}

// ---- sync mode -----------------------------------------------------------------------------------------------
void link_sync_run(void)
{
    ESP_LOGI(TAG, "sync mode");
    bsp_display_start();
    ui_sync_start();
    esp_err_t card_err = bsp_sd_init();
    if (bsp_link_start() != ESP_OK) {
        restart();
    }
    size_t cap = LINK_OVERHEAD + LINK_MAX_PAYLOAD;
    uint8_t *rxbuf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    uint8_t *dma = heap_caps_malloc(BSP_SD_MAX_SECTORS * LINK_SECTOR, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *in = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!rxbuf || !dma || !in) {
        ESP_LOGE(TAG, "no memory for sync buffers");
        restart();
    }
    link_rx_t rx;
    link_rx_init(&rx, rxbuf, cap);
    uint32_t total = 0, written = 0, writes = 0, failures = 0;
    int shown = -1;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int n = bsp_link_read(in, 4096, 1000);
        if (n <= 0) {
            if (xTaskGetTickCount() - last > pdMS_TO_TICKS(SYNC_IDLE_MS)) {
                ESP_LOGW(TAG, "no sync traffic for %d s: back to the player", SYNC_IDLE_MS / 1000);
                restart();
            }
            continue;
        }
        size_t off = 0;
        while (off < (size_t)n) {
            size_t used;
            link_frame_t f;
            bool got = link_rx_feed(&rx, in + off, (size_t)n - off, &used, &f);
            off += used;
            if (!got) {
                continue;
            }
            last = xTaskGetTickCount();
            if (f.type == 'I') {
                uint32_t w[5] = {LINK_VERSION, LINK_MODE_SYNC, LINK_SOURCE_NONE,
                                 card_err == ESP_OK ? bsp_sd_sectors() : 0, 0};
                reply('i', w, 5);
            } else if (f.type == 'S' && f.len == 4) {
                total = link_get_u32(f.payload);
                written = writes = failures = 0;
                memset(dma, 0, LINK_SECTOR);
                esp_err_t e = card_err != ESP_OK ? card_err : bsp_sd_write_verified(0, dma, 1);   // card invalid now
                ESP_LOGI(TAG, "start: %u sectors to write; sector 0 cleared: %s", (unsigned)total, esp_err_to_name(e));
                uint32_t w[1] = {(uint32_t)e};
                reply('s', w, 1);
                ui_sync_progress(0);
                shown = 0;
            } else if (f.type == 'W' && f.len > 4 && (f.len - 4) % LINK_SECTOR == 0) {
                uint32_t first = link_get_u32(f.payload);
                uint32_t count = (f.len - 4) / LINK_SECTOR;
                esp_err_t e = ESP_ERR_INVALID_ARG;
                if (card_err != ESP_OK) {
                    e = card_err;
                } else if (count <= BSP_SD_MAX_SECTORS) {
                    memcpy(dma, f.payload + 4, f.len - 4);
                    e = bsp_sd_write_verified(first, dma, count);
                }
                writes++;
                if (e == ESP_OK) {
                    written += count;
                } else if (failures++ < 10) {
                    ESP_LOGE(TAG, "write %u+%u: %s", (unsigned)first, (unsigned)count, esp_err_to_name(e));
                }
                uint32_t w[3] = {first, count, (uint32_t)e};
                reply('w', w, 3);
                int pct = total ? (int)((uint64_t)written * 100 / total) : -1;
                if (pct != shown) {
                    ui_sync_progress(pct > 100 ? 100 : pct);
                    shown = pct;
                }
            } else if (f.type == 'X') {
                ESP_LOGI(TAG, "done: %u of %u sectors in %u writes, %u failed; restarting as the player",
                         (unsigned)written, (unsigned)total, (unsigned)writes, (unsigned)failures);
                reply('x', NULL, 0);
                restart();
            }
        }
    }
}
