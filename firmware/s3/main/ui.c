#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "logic/battery.h"
#include "logic/controls.h"
#include "player_task.h"
#include "settings.h"
#include "ui_fonts.h"

static const char *TAG = "ui";

#define UI_REFRESH_MS     100
#define CONTROLS_MS       20
#define VOLUME_SAVE_MS    2000   // save volume this long after the last change (limits flash writes)
#define LIST_IDLE_MS      6000
#define REVEAL_TIMEOUT_MS 1500   // light the screen by then even if the player hasn't reported a tune
#define FADE_IN_MS        180
#define BATTERY_SAMPLE_MS 2000
#define BATTERY_LOG_MS    60000
#define MAX_SPEED_PILLS   5      // more speeds than this: one "Speed n/N" button that steps through them

// Tune list geometry: big rows (a finger is ~50 px on this screen), the chosen row in the centre.
#define ROW_H             64
#define ROW_W             320
#define TITLE_MAX_W       260
#define LIST_FADE_PX      160    // rows this far from the centre are at their dimmest
#define LIST_MOVING_MS    100    // the list moved this recently: a touch stops it rather than choosing a tune

#define COL_BG            lv_color_hex(0x000000)
#define COL_TEXT          lv_color_hex(0xF2F2F2)
#define COL_DIM           lv_color_hex(0x8C8C8C)
#define COL_HINT          lv_color_hex(0x5A5A5A)
#define COL_BATT          lv_color_hex(0x6A6A6A)   // battery outline: subtle
#define COL_PLUGGED       lv_color_hex(0x2EC4B6)
#define COL_LOW           lv_color_hex(0xF2A33A)
#define COL_EMPTY         lv_color_hex(0xFF5A5F)
#define COL_RING_BG       lv_color_hex(0x262626)
#define COL_ACCENT        lv_color_hex(0xF2A33A)   // warm amber
#define COL_VOLUME        lv_color_hex(0xB388FF)   // violet: the ring while in volume mode

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
static lv_obj_t *s_scr, *s_ring, *s_title, *s_time, *s_sub, *s_icon, *s_glow_outer, *s_glow_inner;
static lv_obj_t *s_speeds;          // row of speed buttons
static uint8_t s_pill_count;
static bool s_pill_steps;           // one button that steps through many speeds
static lv_obj_t *s_list_btn;
static lv_obj_t *s_batt, *s_batt_fill, *s_batt_bolt;
static battery_t s_battery;
static lv_obj_t *s_list, *s_col;
static lv_obj_t **s_rows;
static uint16_t s_row_count;
static bool s_list_open;
static uint32_t s_list_input_ms;
static int32_t s_list_sel;          // row the list is at (or is animating to)
static uint32_t s_list_scroll_ms;   // last time the list moved (flick, snap or knob)
static bool s_list_knob_target;     // s_list_sel was set by the knob; don't overwrite it from a half-way scroll
static bool s_press_while_moving;   // this touch landed on a moving list: it stops it, it doesn't choose
static knob_seek_t s_seek;
static volume_mode_t s_vol_mode;
static tap_detector_t s_taps;
static resume_saver_t s_saver;
static int32_t s_volume;
static bool s_volume_dirty;
static uint32_t s_volume_changed_ms;
static bool s_gesture_this_press;
static player_status_t s_shown;
static bool s_revealed;
static uint32_t s_start_ms;

static void text_for_font(char *dst, size_t n, const char *utf8)
{
#if UI_FOLD_ACCENTS
    text_fold_ascii(dst, n, utf8);
#else
    strlcpy(dst, utf8, n);
#endif
}

// ---- tune list ----------------------------------------------------------------------------------------------
// A vertical column of rows that snaps a row to the centre. Rows fade out towards the top and bottom edges of the
// round screen, so there are no hard edges or highlight bars: the centre row is simply the brightest.
static void list_restyle(void)
{
    lv_area_t c;
    lv_obj_get_coords(s_col, &c);
    lv_coord_t mid = (lv_coord_t)((c.y1 + c.y2) / 2);
    for (uint16_t i = 0; i < s_row_count; i++) {
        lv_area_t a;
        lv_obj_get_coords(s_rows[i], &a);
        int32_t d = LV_ABS((a.y1 + a.y2) / 2 - mid);
        lv_opa_t opa = d >= LIST_FADE_PX ? 30 : (lv_opa_t)(255 - d * 225 / LIST_FADE_PX);
        if (lv_obj_get_style_opa(s_rows[i], 0) != opa) {
            lv_obj_set_style_opa(s_rows[i], opa, 0);
        }
    }
}

static int32_t list_row_at_scroll(void)
{
    int32_t i = (lv_obj_get_scroll_y(s_col) + ROW_H / 2) / ROW_H;
    return i < 0 ? 0 : i >= s_row_count ? s_row_count - 1 : i;
}

static void list_hidden_cb(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void anim_opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v); }

static void list_animate(bool in)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_list);
    lv_anim_set_time(&a, in ? 180 : 140);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, in ? LV_OPA_TRANSP : LV_OPA_COVER, in ? LV_OPA_COVER : LV_OPA_TRANSP);
    if (!in) {
        lv_anim_set_ready_cb(&a, list_hidden_cb);
    }
    lv_anim_start(&a);
    if (in) {   // rise slightly into place
        lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_values(&a, 40, 0);
        lv_anim_set_ready_cb(&a, NULL);
        lv_anim_start(&a);
    }
}

static void list_close(const char *why)
{
    ESP_LOGI(TAG, "tune list closed (%s)", why);
    s_list_open = false;
    list_animate(false);
}

static void row_event(lv_event_t *e);

static bool list_build(uint16_t count)
{
    s_rows = calloc(count, sizeof(*s_rows));
    if (!s_rows) {
        return false;
    }
    for (uint16_t i = 0; i < count; i++) {
        char title[TUNE_INDEX_MAX_TITLE + 1], shown[TUNE_INDEX_MAX_TITLE + 1];
        uint32_t colour = 0x8C8C8C;
        if (!player_tune_info(i, title, sizeof(title), &colour)) {
            strlcpy(title, "?", sizeof(title));
        }
        text_for_font(shown, sizeof(shown), title);

        lv_obj_t *row = lv_obj_create(s_col);
        lv_obj_remove_style_all(row);   // no theme background, border or padding
        lv_obj_set_size(row, ROW_W, ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_user_data(row, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(row, row_event, LV_EVENT_ALL, NULL);

        lv_obj_t *dot = lv_obj_create(row);   // the tune's theme colour
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(colour), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *l = lv_label_create(row);
        lv_obj_set_style_text_font(l, UI_FONT_TEXT, 0);
        lv_obj_set_style_text_color(l, COL_TEXT, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_label_set_text(l, shown);
        lv_point_t sz;
        lv_txt_get_size(&sz, shown, UI_FONT_TEXT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_width(l, sz.x > TITLE_MAX_W ? TITLE_MAX_W : sz.x);
        s_rows[i] = row;
    }
    s_row_count = count;
    return true;
}

static void list_open(void)
{
    player_status_t st;
    player_status_get(&st);
    if (st.tune_count == 0 || (!s_rows && !list_build(st.tune_count))) {
        return;
    }
    s_list_sel = st.tune < s_row_count ? st.tune : 0;
    s_list_knob_target = false;
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_col, s_list_sel * ROW_H, LV_ANIM_OFF);
    list_restyle();
    list_animate(true);
    s_list_open = true;
    s_list_input_ms = lv_tick_get();
    ESP_LOGI(TAG, "tune list open (%u tunes)", (unsigned)st.tune_count);
}

static void list_touched(void)
{
    s_list_input_ms = lv_tick_get();
    s_list_knob_target = false;
    s_press_while_moving = lv_tick_get() - s_list_scroll_ms < LIST_MOVING_MS;
}

static void row_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        list_touched();
    } else if (code == LV_EVENT_CLICKED && s_list_open) {   // LVGL sends no click after a drag that scrolled
        if (s_press_while_moving) {
            ESP_LOGI(TAG, "tap stopped the list");
            return;
        }
        uint16_t i = (uint16_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        player_status_t st;
        player_status_get(&st);
        if (i != st.tune) {
            player_task_select_tune(i);
        }
        list_close(i != st.tune ? "tune chosen" : "current tune tapped");
    }
}

static void col_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:   // on the padding above the first / below the last row
        list_touched();
        break;
    case LV_EVENT_SCROLL:
        list_restyle();
        s_list_input_ms = s_list_scroll_ms = lv_tick_get();
        break;
    case LV_EVENT_SCROLL_END:
        if (!s_list_knob_target) {
            s_list_sel = list_row_at_scroll();
        }
        list_restyle();
        break;
    default:
        break;
    }
}

static void list_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE && s_list_open) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            list_close("swipe");
        }
    }
}

// ---- main screen input --------------------------------------------------------------------------------------
static void screen_event(lv_event_t *e)
{
    // Only touches on the main screen itself. Events bubbling up from the list (e.g. the swipe that closes it) and
    // anything while the list is open are not for us.
    if (s_list_open || lv_event_get_target(e) != s_scr) {
        return;
    }
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_gesture_this_press = false;
        break;
    case LV_EVENT_GESTURE: {
        // Swipes do nothing on the main screen, but a swipe must not count as a tap (play/pause) either.
        s_gesture_this_press = true;
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
            int32_t sel = s_list_sel + delta, last = (int32_t)s_row_count - 1;
            s_list_sel = sel < 0 ? 0 : sel > last ? last : sel;
            s_list_knob_target = true;
            lv_obj_scroll_to_y(s_col, s_list_sel * ROW_H, LV_ANIM_ON);
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
// Speed buttons: one pill per speed ("Slow  Medium  Fast"); the current one filled in its speed colour.
static void pill_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_list_open) {
        return;
    }
    uint8_t i = (uint8_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    uint8_t to = s_pill_steps ? (uint8_t)((s_shown.speed + 1u) % s_shown.speed_count) : i;
    ESP_LOGI(TAG, "speed button: %u of %u", (unsigned)to + 1u, (unsigned)s_shown.speed_count);
    player_task_play_speed(to);
}

static void speeds_build(uint8_t count)
{
    lv_obj_clean(s_speeds);
    s_pill_steps = count > MAX_SPEED_PILLS;
    s_pill_count = count <= 1 ? 0 : s_pill_steps ? 1 : count;
    for (uint8_t i = 0; i < s_pill_count; i++) {
        lv_obj_t *p = lv_obj_create(s_speeds);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, LV_SIZE_CONTENT, 36);
        lv_obj_set_style_pad_hor(p, 13, 0);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p, COL_TEXT, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(p, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(p, 6);   // 48 px tall to the finger
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(p, (void *)(uintptr_t)i);
        lv_obj_add_event_cb(p, pill_event, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(p);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, 0);
        const char *name = speed_name(i, count);
        if (name && !s_pill_steps) {
            lv_label_set_text(l, name);
        } else {
            lv_label_set_text_fmt(l, "%u", (unsigned)i + 1u);
        }
        lv_obj_center(l);
    }
}

static void speeds_style(uint8_t speed, uint8_t count)
{
    for (uint8_t i = 0; i < s_pill_count; i++) {
        lv_obj_t *p = lv_obj_get_child(s_speeds, i);
        lv_obj_t *l = lv_obj_get_child(p, 0);
        bool on = s_pill_steps || i == speed;
        lv_color_t c = speed_colour(s_pill_steps ? speed : i, count);
        lv_obj_set_style_bg_color(p, c, 0);
        lv_obj_set_style_bg_opa(p, on ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(l, on ? c : COL_DIM, 0);
        if (s_pill_steps) {
            lv_label_set_text_fmt(l, "Speed %u/%u", (unsigned)speed + 1u, (unsigned)count);
        }
    }
}

static void list_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !s_list_open) {
        list_open();
    }
}

// Battery icon: outline, a fill for the level, a small bolt when on USB (the pin then reads USB, not the cell).
static void battery_show(void)
{
    static battery_state_t shown;
    static bool any;
    static uint32_t logged_ms;
    uint32_t mv = bsp_supply_mv();
    battery_state_t b = battery_update(&s_battery, mv);
    uint32_t now = lv_tick_get();
    if (!b.known) {
        return;
    }
    bool changed = !any || b.plugged != shown.plugged || b.level != shown.level;
    if (changed || now - logged_ms >= BATTERY_LOG_MS) {
        if (b.plugged) {
            ESP_LOGI(TAG, "battery: on USB (%u mV)", (unsigned)mv);
        } else {
            ESP_LOGI(TAG, "battery: %u mV, level %u%%", (unsigned)mv, (unsigned)b.level);
        }
        logged_ms = now;
    }
    if (!changed) {
        return;
    }
    any = true;
    shown = b;
    lv_obj_clear_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    if (b.plugged) {
        lv_obj_add_flag(s_batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_batt_bolt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_coord_t w = (lv_coord_t)(b.level * 18u / 100u);
    lv_obj_set_width(s_batt_fill, w < 2 ? 2 : w);
    lv_obj_set_style_bg_color(s_batt_fill, b.level <= 10 ? COL_EMPTY : b.level <= 20 ? COL_LOW : COL_DIM, 0);
}

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
    bool speed_changed = first || st.speed != s_shown.speed || st.speed_count != s_shown.speed_count ||
                         st.tune_id != s_shown.tune_id;
    if (first || st.speed_count != s_shown.speed_count || st.tune_id != s_shown.tune_id) {
        speeds_build(st.speed_count);
    }
    if (speed_changed) {
        speeds_style(st.speed, st.speed_count);
    }
    if (first || st.tune_count != s_shown.tune_count) {
        if (st.tune_count > 1) {
            lv_obj_clear_flag(s_list_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_list_btn, LV_OBJ_FLAG_HIDDEN);
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

    static uint32_t battery_ms;
    static bool battery_once;
    if (!battery_once || now - battery_ms >= BATTERY_SAMPLE_MS) {
        battery_once = true;
        battery_ms = now;
        battery_show();
    }

    // Boot: light the screen only once it shows the tune (or an error), so there's no flash of an empty or white
    // screen. Draw the frame now, then fade the backlight in.
    if (!s_revealed && ((st.state != PLAYER_IDLE && st.title[0]) || now - s_start_ms >= REVEAL_TIMEOUT_MS)) {
        s_revealed = true;
        lv_refr_now(NULL);
        bsp_backlight_fade_in(FADE_IN_MS);
        ESP_LOGI(TAG, "screen on at %u ms after boot (%s)", (unsigned)esp_log_timestamp(),
                 st.state != PLAYER_IDLE ? "tune shown" : "timeout");
    }

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

// A thin rounded line drawn with lv_line (no font glyph needed). `pts` must stay valid: LVGL keeps the pointer.
static lv_obj_t *line(lv_obj_t *parent, const lv_point_t *pts, uint16_t n, lv_coord_t width, lv_color_t color,
                      lv_align_t align, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, n);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, color, 0);
    lv_obj_align(l, align, x, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent)   // an lv_obj with no theme styling, not clickable or scrollable
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void ui_start(int32_t volume)
{
    static const lv_point_t pts_bar[] = {{0, 0}, {22, 0}};
    static const lv_point_t pts_bolt[] = {{11, 0}, {7, 5}, {12, 5}, {8, 10}};

    s_volume = volume;
    battery_init(&s_battery);
    s_start_ms = lv_tick_get();
    knob_seek_init(&s_seek);
    uint32_t id, ms;
    if (settings_resume(&id, &ms)) {
        resume_saver_init(&s_saver, lv_tick_get(), id, ms);   // don't rewrite what NVS already holds
    }
    if (!bsp_display_lock(-1)) {
        return;
    }
    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, COL_BG, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);   // so a swipe is a gesture, not a scroll
    lv_obj_add_event_cb(s_scr, screen_event, LV_EVENT_ALL, NULL);

    // Progress ring around the edge of the round screen: starts at the top, runs clockwise.
    s_ring = lv_arc_create(s_scr);
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
    s_glow_outer = lv_obj_create(s_scr);
    s_glow_inner = lv_obj_create(s_scr);
    lv_obj_t *discs[2] = {s_glow_outer, s_glow_inner};
    const lv_coord_t sizes[2] = {280, 190};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(discs[i], sizes[i], sizes[i]);
        lv_obj_align(discs[i], LV_ALIGN_CENTER, 0, -20);
        lv_obj_set_style_radius(discs[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(discs[i], LV_OPA_10, 0);
        lv_obj_set_style_border_width(discs[i], 0, 0);
        lv_obj_clear_flag(discs[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_move_background(s_glow_inner);
    lv_obj_move_background(s_glow_outer);

    // Battery, top centre: small and dim.
    s_batt = plain(s_scr);
    lv_obj_set_size(s_batt, 25, 12);
    lv_obj_align(s_batt, LV_ALIGN_CENTER, 0, -157);
    lv_obj_t *body = plain(s_batt);
    lv_obj_set_size(body, 22, 12);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, COL_BATT, 0);
    lv_obj_set_style_border_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_t *nub = plain(s_batt);
    lv_obj_set_size(nub, 2, 4);
    lv_obj_set_pos(nub, 23, 4);
    lv_obj_set_style_bg_color(nub, COL_BATT, 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    s_batt_fill = plain(body);
    lv_obj_set_size(s_batt_fill, 2, 8);
    lv_obj_set_pos(s_batt_fill, 1, 1);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    s_batt_bolt = line(body, pts_bolt, 4, 2, COL_PLUGGED, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);   // until the first reading

    // Speed buttons under the battery.
    s_speeds = plain(s_scr);
    lv_obj_set_size(s_speeds, 250, 48);
    lv_obj_align(s_speeds, LV_ALIGN_CENTER, 0, -124);
    lv_obj_set_flex_flow(s_speeds, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_speeds, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_speeds, 6, 0);

#if UI_HAVE_ICONS
    s_icon = label(s_scr, UI_FONT_ICON, COL_ACCENT, -76);
#endif
    s_title = label(s_scr, UI_FONT_TEXT, COL_TEXT, -26);
    lv_obj_set_width(s_title, 290);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    s_time = label(s_scr, UI_FONT_TIME, COL_TEXT, 34);
    s_sub = label(s_scr, UI_FONT_SMALL, COL_DIM, 78);

    // Tune list button at the bottom: three thin lines, no background; dims while pressed.
    s_list_btn = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list_btn);
    lv_obj_set_size(s_list_btn, 64, 48);
    lv_obj_align(s_list_btn, LV_ALIGN_CENTER, 0, 126);
    lv_obj_clear_flag(s_list_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(s_list_btn, LV_OPA_40, LV_STATE_PRESSED);
    for (int i = -1; i <= 1; i++) {
        line(s_list_btn, pts_bar, 2, 2, COL_DIM, LV_ALIGN_CENTER, 0, (lv_coord_t)(i * 7));
    }
    lv_obj_add_event_cb(s_list_btn, list_button_event, LV_EVENT_CLICKED, NULL);

    // Tune list: full screen on top, hidden until the list button. Rows are built on first open.
    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_list, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_list, list_event, LV_EVENT_GESTURE, NULL);
    s_col = lv_obj_create(s_list);
    lv_obj_remove_style_all(s_col);
    lv_obj_set_size(s_col, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(s_col);
    lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(s_col, (BSP_LCD_V_RES - ROW_H) / 2, 0);   // so the first and last rows can centre
    lv_obj_set_scroll_dir(s_col, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_col, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_col, col_event, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);

    lv_timer_t *refresh = lv_timer_create(ui_refresh, UI_REFRESH_MS, NULL);
    lv_timer_ready(refresh);   // first run right away
    lv_timer_create(controls_poll, CONTROLS_MS, NULL);
    bsp_display_unlock();
    ESP_LOGI(TAG, "fonts: %s", UI_FOLD_ACCENTS ? "built-in ASCII (accents folded; run tools/gen_fonts.sh)"
                                                : "Latin-1 (fadas)");
}
