#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "logic/battery.h"
#include "logic/controls.h"
#include "logic/look.h"
#include "player_task.h"
#include "settings.h"
#include "ui_fonts.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "ui";

#define UI_REFRESH_MS     100
#define CONTROLS_MS       20
#define VOLUME_SAVE_MS    2000   // save volume this long after the last change (limits flash writes)
#define LIST_IDLE_MS      8000
#define REVEAL_TIMEOUT_MS 1500   // light the screen by then even if the player hasn't reported a tune
#define FADE_IN_MS        180
#define BATTERY_SAMPLE_MS 2000
#define BATTERY_LOG_MS    60000
#define RING_INNER_R      170    // inner edge of the perimeter ring (352 px arc, 6 px wide)

// Tune screen layout (the list pages use the same positions). y offsets from the centre.
#define ICON_Y            -76
#define TITLE_Y           -26
#define TITLE_W           290
#define TIME_Y            34
#define SUB_Y             78
#define GLOW_Y            -20

// Speed scale: an arc concentric with the ring, from SCALE_SPAN_D10/2 left of straight up to as far right.
#define SCALE_R           130    // px from the centre
#define SCALE_SPAN_D10    760    // 76 degrees
#define SCALE_GAP_D10     30     // 3 degrees between segments
#define SCALE_LABEL_D10   500    // "Slow" / "Fast" sit 50 degrees round, so they follow the curve down
#define SCALE_BAND        22     // touch band either side of the arc
#define SCALE_TRACK_W     4
#define SCALE_DOT         12

#define PAGE_H            BSP_LCD_V_RES   // tune list: one page per tune
#define LIST_MOVING_MS    100    // the list moved this recently: a touch stops it rather than choosing a tune

#define COL_BG            lv_color_hex(0x000000)
#define COL_TEXT          lv_color_hex(0xF2F2F2)
#define COL_DIM           lv_color_hex(0x8C8C8C)
#define COL_HINT          lv_color_hex(0x5A5A5A)
#define COL_RING_BG       lv_color_hex(0x262626)
#define COL_ACCENT        lv_color_hex(0xF2A33A)   // warm amber
#define COL_VOLUME        lv_color_hex(0xB388FF)   // violet: the ring while in volume mode
#define COL_BATT          lv_color_hex(0x6A6A6A)   // battery outline: subtle
#define COL_PLUGGED       lv_color_hex(0x2EC4B6)
#define COL_LOW           lv_color_hex(0xF2A33A)
#define COL_EMPTY         lv_color_hex(0xFF5A5F)

#if UI_FOLD_ACCENTS
#define SEP " - "
#else
#define SEP "  \xC2\xB7  "   // middle dot (Latin-1, in the generated font)
#endif

static lv_color_t speed_colour(uint8_t index, uint8_t count) { return lv_color_hex(speed_rgb(index, count)); }

// Touched only from the LVGL task (timers and event callbacks).
static lv_obj_t *s_scr, *s_ring, *s_title, *s_time, *s_sub, *s_icon, *s_glow[2];
static lv_obj_t *s_scale, *s_scale_slow, *s_scale_fast, *s_scale_dot, *s_scale_seg[TUNE_INDEX_MAX_MARKERS];
static uint8_t s_scale_count;
static bool s_scale_gesture;        // this touch on the scale became a swipe: not a tap
static lv_obj_t *s_list_btn;
static lv_obj_t *s_batt, *s_batt_fill, *s_batt_bolt;
static battery_t s_battery;

static lv_obj_t *s_list, *s_col, *s_list_arc;
static lv_obj_t **s_rows;
static player_tune_brief_t *s_briefs;
static uint16_t s_tune_rows;        // rows 0..s_tune_rows-1 are tunes; the last row is the guide
static uint16_t s_row_count;
static int32_t s_spot_row = -1;     // page the list ring is coloured for
static bool s_list_open;
static uint32_t s_list_input_ms;
static int32_t s_list_sel;          // row the list is at (or is animating to)
static uint32_t s_list_scroll_ms;   // last time the list moved (flick, snap or knob)
static bool s_list_knob_target;     // s_list_sel was set by the knob; don't overwrite it from a half-way scroll
static bool s_press_while_moving;   // this touch landed on a moving list: it stops it, it doesn't choose
static bool s_list_gesture;         // this touch on the list became a swipe: not a tap

static lv_obj_t *s_guide;
static bool s_guide_open;

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

static lv_obj_t *plain(lv_obj_t *parent)   // an lv_obj with no theme styling, not clickable or scrollable
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
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

static const lv_point_t k_bar[] = {{0, 0}, {22, 0}};   // one bar of the "three lines" list icon

static lv_obj_t *list_icon(lv_obj_t *parent, lv_color_t c, lv_align_t align, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *box = plain(parent);
    lv_obj_set_size(box, 24, 18);
    lv_obj_align(box, align, x, y);
    for (int i = -1; i <= 1; i++) {
        line(box, k_bar, 2, 2, c, LV_ALIGN_CENTER, 0, (lv_coord_t)(i * 7));
    }
    return box;
}

static void glow_discs(lv_obj_t *parent, lv_obj_t *out[2], lv_coord_t y)
{
    const lv_coord_t sizes[2] = {280, 190};
    for (int i = 0; i < 2; i++) {
        out[i] = plain(parent);
        lv_obj_set_size(out[i], sizes[i], sizes[i]);
        lv_obj_align(out[i], LV_ALIGN_CENTER, 0, y);
        lv_obj_set_style_radius(out[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(out[i], LV_OPA_10, 0);
    }
}

static void anim_opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }
static void hide_when_done(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

// Slide a full-screen layer in from the right (or back out to the right), fading.
static void slide(lv_obj_t *layer, bool in)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, layer);
    lv_anim_set_time(&a, in ? 200 : 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, in ? LV_OPA_TRANSP : LV_OPA_COVER, in ? LV_OPA_COVER : LV_OPA_TRANSP);
    if (!in) {
        lv_anim_set_ready_cb(&a, hide_when_done);
    }
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, in ? 60 : 0, in ? 0 : 60);
    lv_anim_set_ready_cb(&a, NULL);
    lv_anim_start(&a);
}

// ---- guide --------------------------------------------------------------------------------------------------
static void guide_close(void)
{
    if (!s_guide_open) {
        return;
    }
    s_guide_open = false;
    slide(s_guide, false);
    ESP_LOGI(TAG, "guide closed");
    if (!settings_guide_seen()) {
        settings_save_guide_seen();
    }
}

static void guide_open(const char *why)
{
    lv_obj_clear_flag(s_guide, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_guide);
    slide(s_guide, true);
    s_guide_open = true;
    ESP_LOGI(TAG, "guide shown (%s)", why);
}

static void guide_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {   // a tap anywhere closes it (and doesn't reach the player)
        guide_close();
    }
}

static void guide_build(void)
{
    static const struct {
        const char *what, *does;
    } k_rows[] = {
        {"Tap", "play / pause"},
        {"Double-tap", "start this speed again"},
        {"Turn the knob", "back / forward 2 s"},
        {"Hold", "volume: turn the knob"},
        {"Slow - Fast", "tap a speed to play it"},
        {"", "all tunes"},                 // left column: the list icon
        {"In the list", "swipe right to go back"},
    };
    s_guide = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_guide);
    lv_obj_set_size(s_guide, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_guide, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_guide, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_guide, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_guide, guide_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *h = label(s_guide, UI_FONT_TEXT, COL_ACCENT, -120);
    lv_label_set_text(h, "How to use");
    const int n = (int)(sizeof(k_rows) / sizeof(k_rows[0]));
    for (int i = 0; i < n; i++) {
        lv_coord_t y = (lv_coord_t)(-80 + i * 27);
        if (k_rows[i].what[0]) {
            lv_obj_t *w = lv_label_create(s_guide);
            lv_obj_set_style_text_font(w, UI_FONT_SMALL, 0);
            lv_obj_set_style_text_color(w, COL_ACCENT, 0);
            lv_label_set_text(w, k_rows[i].what);
            lv_obj_align(w, LV_ALIGN_RIGHT_MID, -(BSP_LCD_H_RES / 2 + 30), y);
        } else {
            list_icon(s_guide, COL_ACCENT, LV_ALIGN_RIGHT_MID, -(BSP_LCD_H_RES / 2 + 30), y);
        }
        lv_obj_t *d = lv_label_create(s_guide);
        lv_obj_set_style_text_font(d, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(d, COL_TEXT, 0);
        lv_label_set_text(d, k_rows[i].does);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, BSP_LCD_H_RES / 2 - 18, y);
    }
    lv_obj_t *f = label(s_guide, UI_FONT_SMALL, COL_HINT, 122);
    lv_label_set_text(f, "Tap to close");
    lv_obj_add_flag(s_guide, LV_OBJ_FLAG_HIDDEN);
}

// ---- tune list ----------------------------------------------------------------------------------------------
// One tune per page, laid out exactly like the tune screen (its icon, title and glow in the same places), so
// choosing one just fades the list away and the tune screen underneath looks the same. Scroll (one page per flick)
// or turn the knob to move; the ring shows where you are in the list, in the page's colour. The last page opens
// the guide.
static void list_restyle(void)
{
    int32_t y = lv_obj_get_scroll_y(s_col), page = (y + PAGE_H / 2) / PAGE_H;
    page = page < 0 ? 0 : page >= s_row_count ? s_row_count - 1 : page;
    if (page != s_spot_row) {
        s_spot_row = page;
        lv_color_t c = page < s_tune_rows ? lv_color_hex(s_briefs[page].colour) : COL_ACCENT;
        lv_obj_set_style_arc_color(s_list_arc, c, LV_PART_INDICATOR);
    }
    // Position in the list: a short arc on the ring, moving smoothly as the list scrolls.
    int32_t span = 360 / s_row_count, total = PAGE_H * (s_row_count - 1);
    int32_t start = total > 0 ? y * (360 - span) / total : 0;
    start = start < 0 ? 0 : start > 360 - span ? 360 - span : start;
    lv_arc_set_angles(s_list_arc, (uint16_t)start, (uint16_t)(start + span));
}

static int32_t list_row_at_scroll(void)
{
    int32_t i = (lv_obj_get_scroll_y(s_col) + PAGE_H / 2) / PAGE_H;
    return i < 0 ? 0 : i >= s_row_count ? s_row_count - 1 : i;
}

static void fade_out(lv_obj_t *layer)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, layer);
    lv_anim_set_time(&a, 220);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_ready_cb(&a, hide_when_done);
    lv_anim_start(&a);
}

// slide_back: slide out to the right (going back); otherwise fade in place (a tune was chosen: the page's icon and
// title are where the tune screen has them, so only the rest of the tune screen appears).
static void list_close(const char *why, bool slide_back)
{
    ESP_LOGI(TAG, "tune list closed (%s)", why);
    s_list_open = false;
    if (slide_back) {
        slide(s_list, false);
    } else {
        fade_out(s_list);
    }
}

static void row_event(lv_event_t *e);

static lv_obj_t *list_page(uint16_t i)
{
    lv_obj_t *page = lv_obj_create(s_col);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, BSP_LCD_H_RES, PAGE_H);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(page, (void *)(uintptr_t)i);
    lv_obj_add_event_cb(page, row_event, LV_EVENT_ALL, NULL);
    if (i >= s_tune_rows) {   // the guide: a ring with a "?" where a tune's icon goes
        lv_obj_t *glow[2];
        glow_discs(page, glow, GLOW_Y);
        lv_obj_set_style_bg_color(glow[0], COL_ACCENT, 0);
        lv_obj_set_style_bg_color(glow[1], COL_ACCENT, 0);
        lv_obj_t *help = plain(page);
        lv_obj_set_size(help, 50, 50);
        lv_obj_align(help, LV_ALIGN_CENTER, 0, ICON_Y);
        lv_obj_set_style_radius(help, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(help, 3, 0);
        lv_obj_set_style_border_color(help, COL_ACCENT, 0);
        lv_obj_set_style_border_opa(help, LV_OPA_COVER, 0);
        lv_obj_t *q = lv_label_create(help);
        lv_obj_set_style_text_font(q, UI_FONT_TEXT, 0);
        lv_obj_set_style_text_color(q, COL_ACCENT, 0);
        lv_label_set_text(q, "?");
        lv_obj_center(q);
        lv_label_set_text(label(page, UI_FONT_TEXT, COL_TEXT, TITLE_Y), "How to use");
        lv_label_set_text(label(page, UI_FONT_SMALL, COL_DIM, SUB_Y), "the controls");
        return page;
    }
    const player_tune_brief_t *b = &s_briefs[i];
    lv_color_t c = lv_color_hex(b->colour);
    lv_obj_t *glow[2];
    glow_discs(page, glow, GLOW_Y);
    lv_obj_set_style_bg_color(glow[0], c, 0);
    lv_obj_set_style_bg_color(glow[1], c, 0);
#if UI_HAVE_ICONS
    char glyph[5];
    text_utf8(b->icon, glyph);
    lv_label_set_text(label(page, UI_FONT_ICON, c, ICON_Y), b->icon ? glyph : "");
#endif
    char shown[TUNE_INDEX_MAX_TITLE + 1], len[16];
    text_for_font(shown, sizeof(shown), b->title);
    lv_obj_t *t = label(page, UI_FONT_TEXT, COL_TEXT, TITLE_Y);
    lv_obj_set_width(t, TITLE_W);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_label_set_text(t, shown);
    text_mmss(len, sizeof(len), b->total_ms);
    lv_label_set_text(label(page, UI_FONT_TIME, COL_DIM, TIME_Y), len);   // length where the time goes
    lv_obj_t *sub = label(page, UI_FONT_SMALL, COL_DIM, SUB_Y);
    lv_obj_set_user_data(sub, (void *)1);   // marks the line list_open rewrites ("Now playing")
    if (b->speed_count > 1) {
        lv_label_set_text_fmt(sub, "%u speeds", (unsigned)b->speed_count);
    }
    return page;
}

static bool list_build(uint16_t tunes)
{
    s_rows = calloc((size_t)tunes + 1u, sizeof(*s_rows));
    s_briefs = heap_caps_calloc(tunes, sizeof(*s_briefs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rows || !s_briefs) {
        free(s_rows);
        free(s_briefs);
        s_rows = NULL;
        s_briefs = NULL;
        return false;
    }
    s_tune_rows = tunes;
    s_row_count = (uint16_t)(tunes + 1u);
    for (uint16_t i = 0; i < tunes; i++) {   // cache: tune_index_get walks the index from the start each time
        if (!player_tune_brief(i, &s_briefs[i])) {
            strlcpy(s_briefs[i].title, "?", sizeof(s_briefs[i].title));
            s_briefs[i].colour = 0x8C8C8C;
        }
    }
    for (uint16_t i = 0; i < s_row_count; i++) {
        s_rows[i] = list_page(i);
    }
    return true;
}

// The sub line of tune page i: "Now playing" on the current tune, else its number of speeds.
static void list_mark_playing(uint16_t playing)
{
    for (uint16_t i = 0; i < s_tune_rows; i++) {
        lv_obj_t *sub = NULL;
        for (uint32_t k = 0; k < lv_obj_get_child_cnt(s_rows[i]); k++) {
            lv_obj_t *ch = lv_obj_get_child(s_rows[i], (int32_t)k);
            if (lv_obj_get_user_data(ch) == (void *)1) {
                sub = ch;
            }
        }
        if (!sub) {
            continue;
        }
        if (i == playing) {
            lv_label_set_text(sub, "Now playing");
            lv_obj_set_style_text_color(sub, lv_color_hex(s_briefs[i].colour), 0);
        } else {
            if (s_briefs[i].speed_count > 1) {
                lv_label_set_text_fmt(sub, "%u speeds", (unsigned)s_briefs[i].speed_count);
            } else {
                lv_label_set_text(sub, "");
            }
            lv_obj_set_style_text_color(sub, COL_DIM, 0);
        }
    }
}

static void list_open(void)
{
    player_status_t st;
    player_status_get(&st);
    if (st.tune_count == 0 || (!s_rows && !list_build(st.tune_count))) {
        return;
    }
    s_list_sel = st.tune < s_tune_rows ? st.tune : 0;
    s_list_knob_target = false;
    s_spot_row = -1;
    list_mark_playing(st.tune);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_col, s_list_sel * PAGE_H, LV_ANIM_OFF);
    list_restyle();
    slide(s_list, true);
    s_list_open = true;
    s_list_input_ms = lv_tick_get();
    ESP_LOGI(TAG, "tune list open (%u tunes)", (unsigned)st.tune_count);
}

static void list_touched(void)
{
    s_list_input_ms = lv_tick_get();
    s_list_knob_target = false;
    s_list_gesture = false;
    s_press_while_moving = lv_tick_get() - s_list_scroll_ms < LIST_MOVING_MS;
}

static void row_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        list_touched();
    } else if (code == LV_EVENT_CLICKED && s_list_open) {   // LVGL sends no click after a drag that scrolled
        if (s_list_gesture) {
            return;   // a sideways swipe: only a right swipe means something (back)
        }
        if (s_press_while_moving) {
            ESP_LOGI(TAG, "tap stopped the list");
            return;
        }
        uint16_t i = (uint16_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        if (i >= s_tune_rows) {
            list_close("guide chosen", false);
            guide_open("from the list");
            return;
        }
        player_status_t st;
        player_status_get(&st);
        if (i != st.tune) {
            player_task_select_tune(i);
        }
        list_close(i != st.tune ? "tune chosen" : "current tune tapped", false);
    }
}

static void col_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
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
        s_list_gesture = true;
        if (lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_RIGHT) {
            list_close("swipe right", true);
        }
    }
}

// ---- speed scale --------------------------------------------------------------------------------------------
// An arc around the top, concentric with the ring: "Slow" at its left end, "Fast" at its right, one segment per
// speed, and a dot that travels along it. Angles in tenths of a degree from straight up (logic/look.h).
static void polar(int32_t d10, int32_t r, lv_coord_t *x, lv_coord_t *y)
{
    float a = (float)d10 * (float)M_PI / 1800.0f;
    *x = (lv_coord_t)lroundf((float)r * sinf(a));
    *y = (lv_coord_t)lroundf(-(float)r * cosf(a));
}

static void scale_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_HIT_TEST: {   // only the band around the arc and its labels; elsewhere is the main screen
        lv_hit_test_info_t *info = lv_event_get_param(e);
        info->res = scale_hit(info->point->x - BSP_LCD_H_RES / 2, info->point->y - BSP_LCD_V_RES / 2, SCALE_R,
                              SCALE_BAND, SCALE_LABEL_D10 + 120);
        break;
    }
    case LV_EVENT_PRESSED:
        s_scale_gesture = false;
        break;
    case LV_EVENT_GESTURE:
        s_scale_gesture = true;
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_get_act();
        if (s_scale_gesture || s_list_open || s_guide_open || !indev || s_scale_count <= 1) {
            break;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t a = angle_d10(p.x - BSP_LCD_H_RES / 2, p.y - BSP_LCD_V_RES / 2);
        uint8_t to = scale_speed_at(a + SCALE_SPAN_D10 / 2, s_scale_count, SCALE_SPAN_D10);
        ESP_LOGI(TAG, "speed scale tapped at %+d.%u deg: speed %u of %u", (int)(a / 10), (unsigned)(LV_ABS(a) % 10),
                 (unsigned)to + 1u, (unsigned)s_scale_count);
        player_task_play_speed(to);
        break;
    }
    default:
        break;
    }
}

static void scale_build(uint8_t count)
{
    s_scale_count = count;
    for (uint8_t i = 0; i < TUNE_INDEX_MAX_MARKERS; i++) {
        if (i >= count || count <= 1) {
            lv_obj_add_flag(s_scale_seg[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int32_t x, w;
        scale_segment(i, count, SCALE_SPAN_D10, SCALE_GAP_D10, &x, &w);
        int32_t a0 = x - SCALE_SPAN_D10 / 2, a1 = a0 + w;   // tenths of a degree from straight up
        // lv_arc: degrees clockwise from its rotation (270 = straight up), 0..359.
        lv_arc_set_bg_angles(s_scale_seg[i], (uint16_t)((a0 / 10 + 360) % 360), (uint16_t)((a1 / 10 + 360) % 360));
        lv_obj_set_style_arc_color(s_scale_seg[i], speed_colour(i, count), LV_PART_MAIN);
        lv_obj_clear_flag(s_scale_seg[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (count <= 1) {
        lv_obj_add_flag(s_scale, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_scale, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scale_show(uint8_t speed, uint8_t count, uint16_t permille, bool speed_changed)
{
    static int32_t shown_d10 = INT32_MIN;
    if (count <= 1) {
        return;
    }
    if (speed_changed) {
        for (uint8_t i = 0; i < count && i < TUNE_INDEX_MAX_MARKERS; i++) {
            lv_obj_set_style_arc_opa(s_scale_seg[i], i == speed ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
        }
        lv_obj_set_style_text_color(s_scale_slow, speed == 0 ? speed_colour(0, count) : COL_DIM, 0);
        lv_obj_set_style_text_color(s_scale_fast, speed == count - 1 ? speed_colour(speed, count) : COL_DIM, 0);
        lv_obj_set_style_border_color(s_scale_dot, speed_colour(speed, count), 0);
        shown_d10 = INT32_MIN;
    }
    int32_t d10 = scale_dot_x(speed, permille, count, SCALE_SPAN_D10, SCALE_GAP_D10) - SCALE_SPAN_D10 / 2;
    if (d10 != shown_d10) {
        lv_coord_t x, y;
        polar(d10, SCALE_R, &x, &y);
        lv_obj_align(s_scale_dot, LV_ALIGN_CENTER, x, y);
        shown_d10 = d10;
    }
}

// Evidence for "nothing touches the ring": the farthest corner of the top row's parts from the screen centre.
static void scale_check_fit(void)
{
    lv_obj_update_layout(s_scr);
    lv_obj_t *parts[] = {s_scale_slow, s_scale_fast, s_batt};
    float worst = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        lv_area_t a;
        lv_obj_get_coords(parts[i], &a);
        const lv_coord_t xs[2] = {a.x1, a.x2}, ys[2] = {a.y1, a.y2};
        for (int k = 0; k < 4; k++) {
            float dx = xs[k & 1] + 0.5f - BSP_LCD_H_RES / 2.0f, dy = ys[k >> 1] + 0.5f - BSP_LCD_V_RES / 2.0f;
            float r = sqrtf(dx * dx + dy * dy);
            worst = r > worst ? r : worst;
        }
    }
    ESP_LOGI(TAG, "top row fits: farthest corner %.0f px from centre (ring inner edge %d px); scale arc r=%d",
             (double)worst, RING_INNER_R, SCALE_R);
}

// ---- main screen input --------------------------------------------------------------------------------------
static void screen_event(lv_event_t *e)
{
    // Only touches on the main screen itself: not ones bubbling up from the list, the scale or a button.
    if (s_list_open || s_guide_open || lv_event_get_target(e) != s_scr) {
        return;
    }
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_gesture_this_press = false;
        break;
    case LV_EVENT_GESTURE:
        // Swipes do nothing on the main screen, but a swipe must not count as a tap (play/pause) either.
        s_gesture_this_press = true;
        break;
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

static void list_button_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !s_list_open && !s_guide_open) {
        list_open();
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
    if (s_guide_open) {
        // knob does nothing while the guide is up
    } else if (s_list_open) {
        if (delta) {
            int32_t sel = s_list_sel + delta, last = (int32_t)s_row_count - 1;
            s_list_sel = sel < 0 ? 0 : sel > last ? last : sel;
            s_list_knob_target = true;
            lv_obj_scroll_to_y(s_col, s_list_sel * PAGE_H, LV_ANIM_ON);
            s_list_input_ms = now;
        }
        if (now - s_list_input_ms >= LIST_IDLE_MS) {
            list_close("idle", true);
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
        lv_obj_set_style_bg_color(s_glow[0], c, 0);
        lv_obj_set_style_bg_color(s_glow[1], c, 0);
    }
    if (first || st.elapsed_ms / 1000u != s_shown.elapsed_ms / 1000u) {
        char buf[16];
        text_mmss(buf, sizeof(buf), st.elapsed_ms);
        lv_label_set_text(s_time, buf);
    }
    bool tune_changed = first || st.tune_id != s_shown.tune_id || st.speed_count != s_shown.speed_count;
    bool speed_changed = tune_changed || st.speed != s_shown.speed;
    if (tune_changed) {
        scale_build(st.speed_count);
    }
    scale_show(st.speed, st.speed_count, st.speed_permille, speed_changed);
    if (first || st.tune_count != s_shown.tune_count) {
        if (st.tune_count > 0) {
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
        if (st.speed_count > 1) {
            scale_check_fit();
        }
    }

    // Remember where we are (NVS), sparingly: logic/controls.h resume_should_save.
    if (st.tune_count && st.state != PLAYER_ERROR && st.state != PLAYER_IDLE &&
        resume_should_save(&s_saver, now, st.tune_id, st.elapsed_ms, st.state == PLAYER_PLAYING)) {
        settings_save_resume(st.tune_id, st.elapsed_ms);
    }
}

// ---- building the screens -----------------------------------------------------------------------------------
static lv_obj_t *ring(lv_obj_t *parent)
{
    lv_obj_t *r = lv_arc_create(parent);
    lv_obj_set_size(r, BSP_LCD_H_RES - 8, BSP_LCD_V_RES - 8);
    lv_obj_center(r);
    lv_arc_set_rotation(r, 270);
    lv_arc_set_bg_angles(r, 0, 360);
    lv_obj_remove_style(r, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);   // an arc is a draggable slider by default
    lv_obj_set_style_arc_width(r, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(r, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r, COL_RING_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(r, COL_ACCENT, LV_PART_INDICATOR);
    return r;
}

static void build_battery(void)
{
    static const lv_point_t pts_bolt[] = {{11, 0}, {7, 5}, {12, 5}, {8, 10}};
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
}

static void build_scale(void)
{
    // A full-screen layer whose hit test only accepts the band around the arc (scale_event LV_EVENT_HIT_TEST).
    s_scale = plain(s_scr);
    lv_obj_set_size(s_scale, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(s_scale);
    lv_obj_add_flag(s_scale, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(s_scale, scale_event, LV_EVENT_ALL, NULL);
    for (uint8_t i = 0; i < TUNE_INDEX_MAX_MARKERS; i++) {
        lv_obj_t *a = lv_arc_create(s_scale);
        lv_obj_set_size(a, 2 * SCALE_R + SCALE_TRACK_W, 2 * SCALE_R + SCALE_TRACK_W);   // stroke centred on SCALE_R
        lv_obj_center(a);
        lv_obj_set_style_pad_all(a, 0, 0);
        lv_arc_set_rotation(a, 270);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, SCALE_TRACK_W, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        s_scale_seg[i] = a;
    }
    lv_coord_t x, y;
    s_scale_slow = lv_label_create(s_scale);
    lv_obj_set_style_text_font(s_scale_slow, UI_FONT_SMALL, 0);
    lv_label_set_text(s_scale_slow, "Slow");
    polar(-SCALE_LABEL_D10, SCALE_R, &x, &y);
    lv_obj_align(s_scale_slow, LV_ALIGN_CENTER, x, y);
    s_scale_fast = lv_label_create(s_scale);
    lv_obj_set_style_text_font(s_scale_fast, UI_FONT_SMALL, 0);
    lv_label_set_text(s_scale_fast, "Fast");
    polar(SCALE_LABEL_D10, SCALE_R, &x, &y);
    lv_obj_align(s_scale_fast, LV_ALIGN_CENTER, x, y);
    s_scale_dot = plain(s_scale);   // where we are: a white dot ringed in the speed's colour
    lv_obj_set_size(s_scale_dot, SCALE_DOT, SCALE_DOT);
    lv_obj_set_style_radius(s_scale_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_scale_dot, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(s_scale_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scale_dot, 2, 0);
    lv_obj_set_style_border_opa(s_scale_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_scale, LV_OBJ_FLAG_HIDDEN);
}

static void build_list(void)
{
    s_list = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_list, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_list, list_event, LV_EVENT_GESTURE, NULL);

    s_list_arc = ring(s_list);

    s_col = lv_obj_create(s_list);
    lv_obj_remove_style_all(s_col);
    lv_obj_set_size(s_col, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(s_col);
    lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_col, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_col, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_flag(s_col, LV_OBJ_FLAG_SCROLL_ONE);   // one tune per flick
    lv_obj_set_scrollbar_mode(s_col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_col, col_event, LV_EVENT_ALL, NULL);
    lv_obj_move_foreground(s_list_arc);   // the ring stays on top of the pages
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
}

void ui_start(int32_t volume)
{
    s_volume = volume;
    battery_init(&s_battery);
    s_start_ms = lv_tick_get();
    knob_seek_init(&s_seek);
    uint32_t id, ms;
    if (settings_resume(&id, &ms)) {
        resume_saver_init(&s_saver, lv_tick_get(), id, ms);   // don't rewrite what NVS already holds
    }
    bool guide_seen = settings_guide_seen();
    if (!bsp_display_lock(-1)) {
        return;
    }
    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, COL_BG, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);   // so a swipe is a gesture, not a scroll
    lv_obj_add_event_cb(s_scr, screen_event, LV_EVENT_ALL, NULL);

    // Progress ring around the edge, glow behind the centre (drawn first so everything else sits on top).
    s_ring = ring(s_scr);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    glow_discs(s_scr, s_glow, GLOW_Y);

    build_battery();
    build_scale();
#if UI_HAVE_ICONS
    s_icon = label(s_scr, UI_FONT_ICON, COL_ACCENT, ICON_Y);
#endif
    s_title = label(s_scr, UI_FONT_TEXT, COL_TEXT, TITLE_Y);
    lv_obj_set_width(s_title, TITLE_W);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
    s_time = label(s_scr, UI_FONT_TIME, COL_TEXT, TIME_Y);
    s_sub = label(s_scr, UI_FONT_SMALL, COL_DIM, SUB_Y);

    // Tune list button at the bottom: three thin lines, no background; dims while pressed.
    s_list_btn = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_list_btn);
    lv_obj_set_size(s_list_btn, 64, 48);
    lv_obj_align(s_list_btn, LV_ALIGN_CENTER, 0, 126);
    lv_obj_clear_flag(s_list_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(s_list_btn, LV_OPA_40, LV_STATE_PRESSED);
    list_icon(s_list_btn, COL_DIM, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(s_list_btn, list_button_event, LV_EVENT_CLICKED, NULL);

    build_list();
    guide_build();
    if (!guide_seen) {
        lv_obj_clear_flag(s_guide, LV_OBJ_FLAG_HIDDEN);   // first start: no slide, it's the first thing drawn
        s_guide_open = true;
        ESP_LOGI(TAG, "guide shown (first start)");
    }

    lv_timer_t *refresh = lv_timer_create(ui_refresh, UI_REFRESH_MS, NULL);
    lv_timer_ready(refresh);   // first run right away
    lv_timer_create(controls_poll, CONTROLS_MS, NULL);
    bsp_display_unlock();
    ESP_LOGI(TAG, "fonts: %s", UI_FOLD_ACCENTS ? "built-in ASCII (accents folded; run tools/gen_fonts.sh)"
                                                : "Latin-1 (fadas)");
}
