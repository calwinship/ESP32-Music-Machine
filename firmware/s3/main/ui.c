#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "logic/controls.h"
#include "player_task.h"
#include "settings.h"
#include "ui_fonts.h"

static const char *TAG = "ui";

#define UI_REFRESH_MS   100
#define CONTROLS_MS     20
#define VOLUME_SAVE_MS  2000   // save volume this long after the last change (limits flash writes)
#define LIST_IDLE_MS    6000
#define TAP_SLOP_PX     12     // a touch that moves further than this is a drag/flick, not a tap

#define COL_BG          lv_color_hex(0x000000)
#define COL_TEXT        lv_color_hex(0xF2F2F2)
#define COL_DIM         lv_color_hex(0x8C8C8C)
#define COL_RING_BG     lv_color_hex(0x262626)
#define COL_ACCENT      lv_color_hex(0xF2A33A)   // warm amber: list highlight, headings
#define COL_VOLUME      lv_color_hex(0xB388FF)   // violet: the ring while in volume mode
#define COL_BUTTON      lv_color_hex(0x1E1E1E)

// Ring and speed-name colour for each speed, slowest first: calm teal, amber, coral red. A tune with 2 speeds uses
// the first and last; a single-speed tune uses amber.
static lv_color_t speed_colour(uint8_t index, uint8_t count)
{
    static const uint32_t three[] = {0x2EC4B6, 0xF2A33A, 0xFF5A5F};
    if (count <= 1) {
        return lv_color_hex(three[1]);
    }
    if (count == 2) {
        return lv_color_hex(index ? three[2] : three[0]);
    }
    return lv_color_hex(three[index < 3 ? index : 2]);
}

// Touched only from the LVGL task (timers and event callbacks).
static lv_obj_t *s_ring, *s_speed, *s_title, *s_time, *s_sub, *s_list_btn, *s_icon, *s_glow_outer, *s_glow_inner;
static lv_obj_t *s_list, *s_roller;
static bool s_list_open;
static uint32_t s_list_input_ms;
static knob_seek_t s_seek;
static volume_mode_t s_vol_mode;
static tap_detector_t s_taps;
static resume_saver_t s_saver;
static int32_t s_volume;
static bool s_volume_dirty;
static uint32_t s_volume_changed_ms;
static bool s_gesture_this_press;
static lv_point_t s_roller_press;
static bool s_roller_dragged;
static player_status_t s_shown;

static void text_for_font(char *dst, size_t n, const char *utf8)
{
#if UI_FOLD_ACCENTS
    text_fold_ascii(dst, n, utf8);
#else
    strlcpy(dst, utf8, n);
#endif
}

// ---- tune list ----------------------------------------------------------------------------------------------
static void list_close(const char *why)
{
    ESP_LOGI(TAG, "tune list closed (%s)", why);
    s_list_open = false;
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
}

static void list_open(void)
{
    player_status_t st;
    player_status_get(&st);
    if (st.tune_count == 0) {
        return;
    }
    if (!lv_roller_get_option_cnt(s_roller) || lv_roller_get_option_cnt(s_roller) != st.tune_count) {
        // Build the options once: "Title 1\nTitle 2\n..."
        size_t cap = (size_t)st.tune_count * (TUNE_INDEX_MAX_TITLE + 2);
        char *opts = heap_caps_malloc(cap, MALLOC_CAP_8BIT);
        if (!opts) {
            return;
        }
        size_t len = 0;
        for (uint16_t i = 0; i < st.tune_count; i++) {
            char title[TUNE_INDEX_MAX_TITLE + 1], shown[TUNE_INDEX_MAX_TITLE + 1];
            if (!player_tune_title(i, title, sizeof(title))) {
                strlcpy(title, "?", sizeof(title));
            }
            text_for_font(shown, sizeof(shown), title);
            len += (size_t)snprintf(opts + len, cap - len, "%s%s", i ? "\n" : "", shown);
        }
        lv_roller_set_options(s_roller, opts, LV_ROLLER_MODE_NORMAL);
        free(opts);
    }
    lv_roller_set_selected(s_roller, st.tune, LV_ANIM_OFF);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    s_list_open = true;
    s_list_input_ms = lv_tick_get();
    ESP_LOGI(TAG, "tune list open (%u tunes)", (unsigned)st.tune_count);
}

static void roller_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_VALUE_CHANGED) {
        s_list_input_ms = lv_tick_get();
    }
    // The roller scrolls on drag itself, so LVGL still reports a quick flick as a "short click" on release, which
    // used to play wherever the finger lifted. Only a touch that stayed put is a tap.
    lv_indev_t *indev = lv_indev_get_act();
    if (code == LV_EVENT_PRESSED && indev) {
        lv_indev_get_point(indev, &s_roller_press);
        s_roller_dragged = false;
    } else if (code == LV_EVENT_PRESSING && indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        if (LV_ABS(p.x - s_roller_press.x) > TAP_SLOP_PX || LV_ABS(p.y - s_roller_press.y) > TAP_SLOP_PX) {
            s_roller_dragged = true;
        }
    }
    if (code == LV_EVENT_SHORT_CLICKED && !s_roller_dragged) {
        uint16_t sel = lv_roller_get_selected(s_roller);
        player_status_t st;
        player_status_get(&st);
        if (sel != st.tune) {
            player_task_select_tune(sel);
        }
        list_close(sel != st.tune ? "tune chosen" : "current tune tapped");
    }
}

static void list_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !s_list_open) {
        list_open();
    }
}

// ---- main screen input --------------------------------------------------------------------------------------
static void screen_event(lv_event_t *e)
{
    if (s_list_open) {
        return;   // the list handles its own input
    }
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_gesture_this_press = false;
        break;
    case LV_EVENT_GESTURE: {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        s_gesture_this_press = true;
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            ESP_LOGI(TAG, "swipe %s", dir == LV_DIR_LEFT ? "left: next speed" : "right: previous speed");
            player_task_change_speed(dir == LV_DIR_LEFT ? 1 : -1);
        }
        break;
    }
    case LV_EVENT_SHORT_CLICKED:   // not sent after a long press
        if (!s_gesture_this_press && tap_detector_tap(&s_taps, lv_tick_get()) == TAP_DOUBLE) {
            ESP_LOGI(TAG, "double-tap: restart speed");
            player_task_restart_speed();
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

static void controls_poll(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    int32_t delta = bsp_knob_take_delta();

    if (tap_detector_poll(&s_taps, now) == TAP_SINGLE) {
        player_task_tap();
    }
    if (s_list_open) {
        if (delta) {
            int32_t sel = (int32_t)lv_roller_get_selected(s_roller) + delta;
            int32_t last = (int32_t)lv_roller_get_option_cnt(s_roller) - 1;
            lv_roller_set_selected(s_roller, (uint16_t)(sel < 0 ? 0 : sel > last ? last : sel), LV_ANIM_ON);
            s_list_input_ms = now;
        }
        if (now - s_list_input_ms >= LIST_IDLE_MS) {
            list_close("idle");
        }
    } else if (volume_mode_active(&s_vol_mode, now)) {
        if (delta) {
            int32_t v = volume_apply(s_volume, delta);
            volume_mode_touch(&s_vol_mode, now);
            if (v != s_volume) {
                s_volume = v;
                bsp_audio_set_gain_q15(volume_gain_q15(v));
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
        settings_save_volume(s_volume);
    }
}

// ---- drawing ------------------------------------------------------------------------------------------------
static void ui_refresh(lv_timer_t *t)
{
    (void)t;
    static bool first = true;
    static int32_t shown_ring = -1, shown_volume = -2;
    uint32_t now = lv_tick_get();
    player_status_t st;
    player_status_get(&st);

    if (first || strcmp(st.title, s_shown.title) != 0) {
        char buf[TUNE_INDEX_MAX_TITLE + 1];
        text_for_font(buf, sizeof(buf), st.title);
        lv_label_set_text(s_title, buf);
    }
    if (first || st.icon != s_shown.icon || st.colour != s_shown.colour) {
        // Tune theme: its icon in its colour, and a soft glow of the same colour behind the centre.
        lv_color_t c = lv_color_hex(st.colour);
        if (s_icon) {
            char glyph[5];
            text_utf8(st.icon, glyph);
            lv_label_set_text(s_icon, st.icon ? glyph : "");
            lv_obj_set_style_text_color(s_icon, c, 0);
        }
        lv_obj_set_style_bg_color(s_glow_outer, c, 0);
        lv_obj_set_style_bg_color(s_glow_inner, c, 0);
    }
    if (first || st.elapsed_ms / 1000u != s_shown.elapsed_ms / 1000u) {
        char buf[16];
        text_mmss(buf, sizeof(buf), st.elapsed_ms);
        lv_label_set_text(s_time, buf);
    }
    bool speed_changed = first || st.speed != s_shown.speed || st.speed_count != s_shown.speed_count;
    if (speed_changed) {
        const char *name = speed_name(st.speed, st.speed_count);
        lv_obj_set_style_text_color(s_speed, speed_colour(st.speed, st.speed_count), 0);
        if (st.speed_count <= 1) {
            lv_label_set_text(s_speed, "");
        } else if (name) {
            lv_label_set_text_fmt(s_speed, "%s  %u/%u", name, (unsigned)st.speed + 1u, (unsigned)st.speed_count);
        } else {
            lv_label_set_text_fmt(s_speed, "Speed %u/%u", (unsigned)st.speed + 1u, (unsigned)st.speed_count);
        }
    }

    // Ring and the line under the time: playback progress through the current speed, or the volume gauge.
    bool vol = volume_mode_active(&s_vol_mode, now);
    int32_t ring = vol ? s_volume * 100 : st.speed_permille;
    int32_t volume_shown = vol ? s_volume : -1;
    if (volume_shown != shown_volume || speed_changed) {
        lv_obj_set_style_arc_color(s_ring, vol ? COL_VOLUME : speed_colour(st.speed, st.speed_count),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_sub, vol ? COL_VOLUME : COL_DIM, 0);
        shown_volume = volume_shown;
        s_shown.total_ms = UINT32_MAX;   // force the sub line to redraw
    }
    if (ring != shown_ring) {
        lv_arc_set_value(s_ring, (int16_t)ring);
        shown_ring = ring;
    }
    if (vol) {
        lv_label_set_text_fmt(s_sub, "Volume %d", (int)s_volume);
    } else if (first || st.total_ms / 1000u != s_shown.total_ms / 1000u) {
        char buf[16];
        text_mmss(buf, sizeof(buf), st.total_ms);
        lv_label_set_text_fmt(s_sub, "of %s", buf);
    }
    s_shown = st;
    first = false;

    // Remember where we are (NVS), sparingly: logic/controls.h resume_should_save.
    if (st.tune_count && st.state != PLAYER_ERROR && st.state != PLAYER_IDLE &&
        resume_should_save(&s_saver, now, st.tune_id, st.elapsed_ms, st.state == PLAYER_PLAYING)) {
        settings_save_resume(st.tune_id, st.elapsed_ms);
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

void ui_start(int32_t volume)
{
    s_volume = volume;
    knob_seek_init(&s_seek);
    uint32_t id, ms;
    if (settings_resume(&id, &ms)) {
        resume_saver_init(&s_saver, lv_tick_get(), id, ms);   // don't rewrite what NVS already holds
    }
    if (!bsp_display_lock(-1)) {
        return;
    }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   // so a swipe is a gesture, not a scroll
    lv_obj_add_event_cb(scr, screen_event, LV_EVENT_ALL, NULL);

    // Progress ring around the edge of the round screen: starts at the top, runs clockwise.
    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, BSP_LCD_H_RES - 8, BSP_LCD_V_RES - 8);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, COL_RING_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, COL_ACCENT, LV_PART_INDICATOR);

    // Glow: two faint discs in the tune's colour, drawn first so everything else sits on top.
    s_glow_outer = lv_obj_create(scr);
    s_glow_inner = lv_obj_create(scr);
    lv_obj_t *discs[2] = {s_glow_outer, s_glow_inner};
    const lv_coord_t sizes[2] = {280, 190};
    const lv_opa_t opas[2] = {LV_OPA_10, LV_OPA_10};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(discs[i], sizes[i], sizes[i]);
        lv_obj_align(discs[i], LV_ALIGN_CENTER, 0, -30);
        lv_obj_set_style_radius(discs[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(discs[i], opas[i], 0);
        lv_obj_set_style_border_width(discs[i], 0, 0);
        lv_obj_clear_flag(discs[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_move_background(s_glow_inner);
    lv_obj_move_background(s_glow_outer);

    s_speed = label(scr, UI_FONT_SMALL, COL_ACCENT, -130);
#if UI_HAVE_ICONS
    s_icon = label(scr, UI_FONT_ICON, COL_ACCENT, -88);
#endif
    s_title = label(scr, UI_FONT_TEXT, COL_TEXT, -36);
    lv_obj_set_width(s_title, 290);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    s_time = label(scr, UI_FONT_TIME, COL_TEXT, 24);
    s_sub = label(scr, UI_FONT_SMALL, COL_DIM, 70);
    // Tune list button at the bottom.
    s_list_btn = lv_btn_create(scr);
    lv_obj_set_size(s_list_btn, 64, 44);
    lv_obj_align(s_list_btn, LV_ALIGN_CENTER, 0, 122);
    lv_obj_set_style_bg_color(s_list_btn, COL_BUTTON, 0);
    lv_obj_set_style_bg_color(s_list_btn, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_list_btn, 22, 0);
    lv_obj_set_style_shadow_width(s_list_btn, 0, 0);
    lv_obj_t *icon = lv_label_create(s_list_btn);
    lv_obj_set_style_text_font(icon, UI_FONT_TEXT, 0);
    lv_obj_set_style_text_color(icon, COL_TEXT, 0);
    lv_label_set_text(icon, LV_SYMBOL_LIST);
    lv_obj_center(icon);
    lv_obj_add_event_cb(s_list_btn, list_button_event, LV_EVENT_CLICKED, NULL);

    // Tune list: a full-screen roller on top, hidden until swipe up.
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(s_list);
    lv_obj_set_style_bg_color(s_list, COL_BG, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *heading = label(s_list, UI_FONT_SMALL, COL_ACCENT, -128);
    lv_label_set_text(heading, "Tunes");
    s_roller = lv_roller_create(s_list);
    lv_obj_set_width(s_roller, 290);
    lv_roller_set_visible_row_count(s_roller, 5);
    lv_obj_center(s_roller);
    lv_obj_set_style_text_font(s_roller, UI_FONT_TEXT, 0);
    lv_obj_set_style_text_line_space(s_roller, 14, 0);
    lv_obj_set_style_bg_opa(s_roller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_roller, 0, 0);
    lv_obj_set_style_text_color(s_roller, COL_DIM, 0);
    lv_obj_set_style_bg_color(s_roller, COL_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller, COL_BG, LV_PART_SELECTED);
    lv_obj_set_style_radius(s_roller, 10, LV_PART_SELECTED);
    lv_obj_add_event_cb(s_roller, roller_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(ui_refresh, UI_REFRESH_MS, NULL);
    lv_timer_create(controls_poll, CONTROLS_MS, NULL);
    bsp_display_unlock();
    ESP_LOGI(TAG, "fonts: %s", UI_FOLD_ACCENTS ? "built-in ASCII (accents folded; run tools/gen_fonts.sh)"
                                                : "Latin-1 (fadas)");
}
