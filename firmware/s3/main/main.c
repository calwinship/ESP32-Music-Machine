// Tune player. Boots into the last tune and position (paused) if NVS has one, else plays the first tune in /tunes
// (written by ./sync-tunes) from its first marker. Controls and screen: ui.h.
#include "bsp/bsp.h"
#include "esp_log.h"
#include "link_task.h"
#include "logic/controls.h"
#include "player_task.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "app";

void app_main(void)
{
    if (link_sync_requested()) {
        link_sync_run();   // never returns
    }
    settings_init();
    int32_t volume = settings_volume();
    bsp_audio_set_gain_q15(volume_gain_q15(volume));
    uint32_t resume_id = 0, resume_ms = 0;
    bool resume = settings_resume(&resume_id, &resume_ms);
    ESP_LOGI(TAG, "volume %d; resume %s", (int)volume, resume ? "point found" : "none");

    ESP_ERROR_CHECK(bsp_display_start());
    if (bsp_storage_mount() != ESP_OK) {
        ESP_LOGE(TAG, "no tunes on the SD card or in flash: the player will report it");
    }
    player_task_start(resume, resume_id, resume_ms);
    ui_start(volume);
    ESP_ERROR_CHECK(bsp_knob_start());
    link_task_start();
    ESP_LOGI(TAG, "ready");
}
