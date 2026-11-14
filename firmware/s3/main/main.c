// Starter app: an arc + number (0..100). Knob clockwise = +1 per detent, anticlockwise = -1; tap the screen = reset to 50.
// Hardware is behind bsp/ (board drivers); the value rules are logic/knob.h (host-tested by ./test).
#include "bsp/bsp.h"
#include "esp_log.h"
#include "logic/knob.h"
#include "lvgl.h"

static const char *TAG = "app";

#define VALUE_MIN     0
#define VALUE_MAX     100
#define VALUE_INITIAL 50

static knob_value_t s_value;
static lv_obj_t *s_arc;
static lv_obj_t *s_label;

// Runs inside the LVGL task, so the display lock is already held.
static void show_value(const char *why)
{
    lv_arc_set_value(s_arc, (int16_t)s_value.value);
    lv_label_set_text_fmt(s_label, "%d", (int)s_value.value);
    ESP_LOGI(TAG, "value=%d (%s)", (int)s_value.value, why);
}

static void knob_poll_timer(lv_timer_t *t)
{
    (void)t;
    int32_t delta = bsp_knob_take_delta();
    if (delta && knob_value_apply(&s_value, delta)) {
        show_value(delta > 0 ? "knob cw" : "knob ccw");
    }
}

static void screen_clicked(lv_event_t *e)
{
    (void)e;
    if (knob_value_set(&s_value, VALUE_INITIAL)) {
        show_value("tap reset");
    } else {
        ESP_LOGI(TAG, "value=%d (tap, unchanged)", (int)s_value.value);
    }
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_event_cb(scr, screen_clicked, LV_EVENT_CLICKED, NULL);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 300, 300);
    lv_obj_center(s_arc);
    lv_arc_set_range(s_arc, VALUE_MIN, VALUE_MAX);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);      // indicator only, not draggable
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);     // taps fall through to the screen

    s_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_center(s_label);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "turn knob / tap to reset");
    lv_obj_set_style_text_color(hint, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 60);

    lv_timer_create(knob_poll_timer, 20, NULL);
}

void app_main(void)
{
    knob_value_init(&s_value, VALUE_MIN, VALUE_MAX, VALUE_INITIAL);
    ESP_ERROR_CHECK(bsp_display_start());
    ESP_ERROR_CHECK(bsp_knob_start());
    if (bsp_display_lock(-1)) {
        ui_create();
        show_value("start");
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "ready");
}
