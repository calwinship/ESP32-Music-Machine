// Tune player. On boot it plays the first tune in /tunes (written by ./sync-tunes) from its first marker.
// Controls: knob = +/-2 s per click; tap = play/pause; swipe left/right = next/previous tune;
// long-press = volume mode (the knob sets volume 0-10 for 3 s, saved in NVS).
// Screen: tune title and elapsed time, plus "Volume N" while in volume mode.
#include <stdint.h>
#include <string.h>

#include "bsp/bsp.h"
#include "esp_log.h"
#include "logic/controls.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "player_task.h"

static const char *TAG = "app";

#define UI_REFRESH_MS   100
#define CONTROLS_MS     20
#define VOLUME_SAVE_MS  2000   // save to NVS this long after the last volume change (limits flash writes)
#define NVS_NAMESPACE   "player"
#define NVS_KEY_VOLUME  "volume"

static lv_obj_t *s_title;
static lv_obj_t *s_time;
static lv_obj_t *s_info;

// Touched only from the LVGL task (timers and event callbacks).
static knob_seek_t s_seek;
static volume_mode_t s_vol_mode;
static int32_t s_volume = VOLUME_DEFAULT;
static bool s_volume_dirty;
static uint32_t s_volume_changed_ms;
static bool s_gesture_this_press;

// ---- volume in NVS ------------------------------------------------------------------------------------------
static void nvs_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition holds data in a layout this IDF can't use (e.g. from the factory firmware): start fresh.
        ESP_LOGW(TAG, "NVS: %s, erasing it", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static int32_t volume_load(void)
{
    nvs_handle_t h;
    int32_t v = VOLUME_DEFAULT;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, NVS_KEY_VOLUME, &v) != ESP_OK || !volume_valid(v)) {
            v = VOLUME_DEFAULT;
        }
        nvs_close(h);
    }
    return v;
}

static void volume_save(int32_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, NVS_KEY_VOLUME, v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "volume %d saved", (int)v);
    } else {
        ESP_LOGE(TAG, "volume save failed: %s", esp_err_to_name(err));
    }
}

static void volume_set(int32_t v)
{
    s_volume = v;
    bsp_audio_set_gain_q15(volume_gain_q15(v));
}

// ---- LVGL timers (display lock held) ------------------------------------------------------------------------
static void controls_poll(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    int32_t delta = bsp_knob_take_delta();
    if (volume_mode_active(&s_vol_mode, now)) {
        if (delta) {
            int32_t v = volume_apply(s_volume, delta);
            volume_mode_touch(&s_vol_mode, now);
            if (v != s_volume) {
                volume_set(v);
                s_volume_dirty = true;
                s_volume_changed_ms = now;
                ESP_LOGI(TAG, "volume %d (gain %u/32767)", (int)v, (unsigned)volume_gain_q15(v));
            }
        }
    } else {
        knob_seek_add(&s_seek, delta, now);
    }
    int32_t clicks;
    if (knob_seek_take(&s_seek, now, &clicks)) {
        player_task_seek(clicks);
    }
    if (s_volume_dirty && now - s_volume_changed_ms >= VOLUME_SAVE_MS) {
        s_volume_dirty = false;
        volume_save(s_volume);
    }
}

static void ui_refresh(lv_timer_t *t)
{
    (void)t;
    static player_status_t shown = {.elapsed_ms = UINT32_MAX};
    static int32_t shown_volume = -1;
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
    bool vol = volume_mode_active(&s_vol_mode, lv_tick_get());
    int32_t want = vol ? s_volume : -1;
    if (want != shown_volume) {
        if (vol) {
            lv_label_set_text_fmt(s_info, "Volume %d", (int)s_volume);
            lv_obj_clear_flag(s_info, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);
        }
        shown_volume = want;
    }
}

// ---- touch ----------------------------------------------------------------------------------------------------
static void screen_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_gesture_this_press = false;
        break;
    case LV_EVENT_GESTURE: {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            s_gesture_this_press = true;
            ESP_LOGI(TAG, "swipe %s", dir == LV_DIR_LEFT ? "left: next tune" : "right: previous tune");
            player_task_change_tune(dir == LV_DIR_LEFT ? 1 : -1);
        }
        break;
    }
    case LV_EVENT_SHORT_CLICKED:   // not sent after a long press
        if (!s_gesture_this_press) {
            player_task_tap();
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        if (!s_gesture_this_press) {
            volume_mode_enter(&s_vol_mode, lv_tick_get());
            ESP_LOGI(TAG, "volume mode (volume %d): turn the knob", (int)s_volume);
        }
        break;
    default:
        break;
    }
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   // so a swipe is a gesture, not a scroll
    lv_obj_add_event_cb(scr, screen_event, LV_EVENT_ALL, NULL);

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

    s_info = lv_label_create(scr);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_info, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(s_info, LV_ALIGN_CENTER, 0, 80);
    lv_obj_add_flag(s_info, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(ui_refresh, UI_REFRESH_MS, NULL);
    lv_timer_create(controls_poll, CONTROLS_MS, NULL);
}

void app_main(void)
{
    nvs_start();
    volume_set(volume_load());
    ESP_LOGI(TAG, "volume %d (gain %u/32767)", (int)s_volume, (unsigned)volume_gain_q15(s_volume));
    knob_seek_init(&s_seek);

    ESP_ERROR_CHECK(bsp_display_start());
    if (bsp_display_lock(-1)) {
        ui_create();
        bsp_display_unlock();
    }
    if (bsp_storage_mount() != ESP_OK) {
        ESP_LOGE(TAG, "no tunes partition: the player will report it");
    }
    player_task_start();
    ESP_ERROR_CHECK(bsp_knob_start());
    ESP_LOGI(TAG, "ready");
}
