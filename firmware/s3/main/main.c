// Tune player: on boot, plays the first tune in /tunes (written by ./sync-tunes) from its first marker.
// Screen: the tune title and elapsed time, nothing else. Tap anywhere = play/pause. The knob is not used yet.
#include <stdint.h>
#include <string.h>

#include "bsp/bsp.h"
#include "esp_log.h"
#include "lvgl.h"
#include "player_task.h"

static const char *TAG = "app";

#define UI_REFRESH_MS 200

static lv_obj_t *s_title;
static lv_obj_t *s_time;

// LVGL timer (display lock held).
static void ui_refresh(lv_timer_t *t)
{
    (void)t;
    static player_status_t shown = {.elapsed_ms = UINT32_MAX};
    player_status_t now;
    player_status_get(&now);
    if (strcmp(now.title, shown.title) != 0) {
        lv_label_set_text(s_title, now.title);
    }
    if (now.elapsed_ms / 1000u != shown.elapsed_ms / 1000u) {
        char buf[16];
        text_mmss(buf, sizeof(buf), now.elapsed_ms);
        lv_label_set_text(s_time, buf);
    }
    shown = now;
}

static void screen_clicked(lv_event_t *e)
{
    (void)e;
    player_task_tap();
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, screen_clicked, LV_EVENT_CLICKED, NULL);

    s_title = lv_label_create(scr);
    lv_obj_set_width(s_title, 280);   // the round screen is narrower away from the centre line
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_label_set_text(s_title, "");
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -60);

    s_time = lv_label_create(scr);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time, lv_color_white(), 0);
    lv_label_set_text(s_time, "0:00");
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 20);

    lv_timer_create(ui_refresh, UI_REFRESH_MS, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_start());
    if (bsp_display_lock(-1)) {
        ui_create();
        bsp_display_unlock();
    }
    if (bsp_storage_mount() != ESP_OK) {
        ESP_LOGE(TAG, "no tunes partition: the player will report it");
    }
    player_task_start();
    ESP_LOGI(TAG, "ready");
}
