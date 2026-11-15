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
#define TITLE_W           280    // at most 2 lines; checked against the disc at boot (title_check_fit)
#define TITLE_SAFE_R      160    // no title corner further than this from the centre
#define TIME_Y            34
#define SUB_Y             78
#define PAUSE_Y           -122

// Timeline ring: the whole tune round the edge, one segment per speed.
#define TL_GAP_D10        40     // 4 degrees between segments
#define TL_WIDTH          6
#define TL_TOUCH_IN       146    // touch band (px from the centre); the ring itself is at 170..176
#define TL_TOUCH_OUT      190

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
#define COL_LOW           lv_color_hex(0xF2A33A)
#define COL_EMPTY         lv_color_hex(0xFF5A5F)

#if UI_FOLD_ACCENTS
#define SEP " - "
#else
#define SEP "  \xC2\xB7  "   // middle dot (Latin-1, in the generated font)
#endif

static lv_color_t speed_colour(uint8_t index, uint8_t count) { return lv_color_hex(speed_rgb(index, count)); }

// Touched only from the LVGL task (timers and event callbacks).
static lv_obj_t *s_scr, *s_title, *s_time, *s_sub, *s_icon, *s_pause;
static lv_obj_t *s_tl, *s_tl_seg[TUNE_INDEX_MAX_MARKERS];
static uint8_t s_tl_count;
static int32_t s_tl_a0[TUNE_INDEX_MAX_MARKERS], s_tl_a1[TUNE_INDEX_MAX_MARKERS];
static uint32_t s_tl_bounds[TUNE_INDEX_MAX_MARKERS + 1];
static int16_t s_tl_shown_end[TUNE_INDEX_MAX_MARKERS];
static bool s_tl_gesture;           // this touch on the ring became a swipe: not a tap
static lv_obj_t *s_list_btn;
static lv_obj_t *s_batt, *s_batt_fill;
static battery_t s_battery;

static lv_obj_t *s_list, *s_col, *s_list_arc;
static lv_obj_t **s_rows;
static player_tune_brief_t *s_briefs;
static uint16_t s_tune_rows;        // rows 0..s_tune_rows-1 are tunes; the last row is the guide
static uint16_t s_row_count;
static int32_t s_spot_row = -1;     // page the list ring was last drawn for
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
    s_spot_row = page;
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
static lv_obj_t *title_label(lv_obj_t *parent);

static lv_obj_t *list_page(uint16_t i)
{
    lv_obj_t *page = lv_obj_create(s_col);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, BSP_LCD_H_RES, PAGE_H);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(page, (void *)(uintptr_t)i);
    lv_obj_add_event_cb(page, row_event, LV_EVENT_ALL, NULL);
    if (i >= s_tune_rows) {   // the guide: a ring with a "?" where a tune's icon goes
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
#if UI_HAVE_ICONS
    lv_color_t c = lv_color_hex(b->colour);   // the tune's colour is on its icon only
    char glyph[5];
    text_utf8(b->icon, glyph);
    lv_label_set_text(label(page, UI_FONT_ICON, c, ICON_Y), b->icon ? glyph : "");
#endif
    char shown[TUNE_INDEX_MAX_TITLE + 1], len[16];
    text_for_font(shown, sizeof(shown), b->title);
    lv_obj_t *t = title_label(page);
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
            lv_obj_set_style_text_color(sub, COL_TEXT, 0);
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

// ---- timeline ring ------------------------------------------------------------------------------------------
// The whole tune once round the edge, starting at the top: one segment per speed, its length in proportion to the
// speed's duration, in the speed's colour; the played part bright, the rest dim (logic/look.h timeline_*).
// Tapping a segment plays that speed from its start.
static uint16_t deg(int32_t d10) { return (uint16_t)(((d10 + 5) / 10) % 360); }

static void timeline_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_HIT_TEST: {   // only the band round the edge; inside it is the main screen
        lv_hit_test_info_t *info = lv_event_get_param(e);
        info->res = ring_hit(info->point->x - BSP_LCD_H_RES / 2, info->point->y - BSP_LCD_V_RES / 2, TL_TOUCH_IN,
                             TL_TOUCH_OUT);
        break;
    }
    case LV_EVENT_PRESSED:
        s_tl_gesture = false;
        break;
    case LV_EVENT_GESTURE:
        s_tl_gesture = true;
        break;
    case LV_EVENT_CLICKED: {
        lv_indev_t *indev = lv_indev_get_act();
        if (s_tl_gesture || s_list_open || s_guide_open || !indev || s_tl_count == 0) {
            break;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t a = angle_d10(p.x - BSP_LCD_H_RES / 2, p.y - BSP_LCD_V_RES / 2);
        uint8_t to = timeline_segment_at(a, s_tl_count, s_tl_a0, s_tl_a1);
        ESP_LOGI(TAG, "ring tapped at %d deg: speed %u of %u", (int)(((a % 3600) + 3600) % 3600 / 10),
                 (unsigned)to + 1u, (unsigned)s_tl_count);
        player_task_play_speed(to);
        break;
    }
    default:
        break;
    }
}

static void timeline_build(const player_status_t *st)
{
    uint8_t n = st->speed_count > TUNE_INDEX_MAX_MARKERS ? TUNE_INDEX_MAX_MARKERS : st->speed_count;
    s_tl_count = st->total_ms ? n : 0;
    memcpy(s_tl_bounds, st->speed_bounds_ms, sizeof(s_tl_bounds));
    if (s_tl_count) {
        timeline_layout(s_tl_bounds, s_tl_count, TL_GAP_D10, s_tl_a0, s_tl_a1);
    }
    for (uint8_t i = 0; i < TUNE_INDEX_MAX_MARKERS; i++) {
        lv_obj_t *a = s_tl_seg[i];
        s_tl_shown_end[i] = -1;
        if (i >= s_tl_count) {
            lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_color_t c = speed_colour(i, s_tl_count);
        lv_arc_set_bg_angles(a, deg(s_tl_a0[i]), deg(s_tl_a1[i]));
        lv_obj_set_style_arc_color(a, c, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_HIDDEN);
    }
}

static void timeline_show(uint8_t speed, uint32_t position_ms)
{
    for (uint8_t i = 0; i < s_tl_count; i++) {
        int32_t end = i < speed ? s_tl_a1[i]
                    : i > speed ? s_tl_a0[i]
                                : timeline_played_d10(s_tl_bounds, i, position_ms, s_tl_a0, s_tl_a1);
        int16_t e = (int16_t)deg(end);
        if (e == s_tl_shown_end[i]) {
            continue;
        }
        s_tl_shown_end[i] = e;
        bool any = e != deg(s_tl_a0[i]);
        lv_obj_set_style_arc_opa(s_tl_seg[i], any ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
        if (any) {
            lv_arc_set_angles(s_tl_seg[i], deg(s_tl_a0[i]), (uint16_t)e);
        }
    }
}

// Title box: centred, wraps, at most 2 lines.
static lv_obj_t *title_label(lv_obj_t *parent)
{
    lv_obj_t *t = label(parent, UI_FONT_TEXT, COL_TEXT, TITLE_Y);
    lv_obj_set_width(t, TITLE_W);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(t, 2 * lv_font_get_line_height(UI_FONT_TEXT), 0);
    return t;
}

// Evidence that every title fits: wrapped at TITLE_W, how many lines, and how far the box's corners reach from the
// centre (the disc inside the ring has radius RING_INNER_R).
static void title_check_fit(uint16_t tunes)
{
    const lv_font_t *f = UI_FONT_TEXT;
    lv_coord_t lh = lv_font_get_line_height(f);
    float worst = 0;
    int too_long = 0;
    char worst_title[TUNE_INDEX_MAX_TITLE + 1] = "";
    for (uint16_t i = 0; i < tunes; i++) {
        player_tune_brief_t b;
        char shown[TUNE_INDEX_MAX_TITLE + 1];
        if (!player_tune_brief(i, &b)) {
            continue;
        }
        text_for_font(shown, sizeof(shown), b.title);
        lv_point_t sz;
        lv_txt_get_size(&sz, shown, f, 0, 0, TITLE_W, LV_TEXT_FLAG_NONE);
        int lines = (sz.y + lh - 1) / lh;
        float h = (float)(lines > 2 ? 2 : lines) * lh;
        float dx = sz.x / 2.0f, dy = -TITLE_Y + h / 2.0f;   // the top corners are the far ones
        float r = sqrtf(dx * dx + dy * dy);
        if (lines > 2) {
            too_long++;
        }
        if (r > worst) {
            worst = r;
            strlcpy(worst_title, b.title, sizeof(worst_title));
        }
        if (lines > 1 || strcmp(b.title, "Over the Water to Charlie") == 0) {
            ESP_LOGI(TAG, "title \"%s\": %d line(s), %d px wide", b.title, lines, (int)sz.x);
        }
    }
    ESP_LOGI(TAG, "titles: %u checked, %d over 2 lines; farthest corner %.0f px (\"%s\"), limit %d, disc %d",
             (unsigned)tunes, too_long, (double)worst, worst_title, TITLE_SAFE_R, RING_INNER_R);
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
// Battery icon: outline and a fill for the level, only while on battery.
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
    if (b.plugged) {   // on USB the pin reads the USB supply, not the cell: show nothing
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    lv_coord_t w = (lv_coord_t)(b.level * 18u / 100u);
    lv_obj_set_width(s_batt_fill, w < 2 ? 2 : w);
    lv_obj_set_style_bg_color(s_batt_fill, b.level <= 10 ? COL_EMPTY : b.level <= 20 ? COL_LOW : COL_DIM, 0);
}

static void ui_refresh(lv_timer_t *t)
{
    (void)t;
    static bool first = true;
    static int32_t shown_volume = -2;
    uint32_t now = lv_tick_get();
    player_status_t st;
    player_status_get(&st);

    if (first || strcmp(st.title, s_shown.title) != 0) {
        char buf[TUNE_INDEX_MAX_TITLE + 1];
        text_for_font(buf, sizeof(buf), st.title);
        lv_label_set_text(s_title, buf);
    }
    if (first || st.icon != s_shown.icon || st.colour != s_shown.colour) {
        // Tune theme: its icon in its colour (the only place the tune's colour appears).
        if (s_icon) {
            char glyph[5];
            text_utf8(st.icon, glyph);
            lv_label_set_text(s_icon, st.icon ? glyph : "");
            lv_obj_set_style_text_color(s_icon, lv_color_hex(st.colour), 0);
        }
    }
    bool paused = st.state == PLAYER_PAUSED || st.state == PLAYER_ENDED;
    if (first || paused != (s_shown.state == PLAYER_PAUSED || s_shown.state == PLAYER_ENDED)) {
        if (paused) {
            lv_obj_clear_flag(s_pause, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pause, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (first || st.elapsed_ms / 1000u != s_shown.elapsed_ms / 1000u) {
        char buf[16];
        text_mmss(buf, sizeof(buf), st.elapsed_ms);
        lv_label_set_text(s_time, buf);
    }
    bool tune_changed = first || st.tune_id != s_shown.tune_id || st.speed_count != s_shown.speed_count ||
                        st.total_ms != s_shown.total_ms;
    bool speed_changed = tune_changed || st.speed != s_shown.speed;
    if (tune_changed) {
        timeline_build(&st);
    }
    timeline_show(st.speed, st.elapsed_ms);
    if (first || st.tune_count != s_shown.tune_count) {
        if (st.tune_count > 0) {
            lv_obj_clear_flag(s_list_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_list_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // The line under the time: the current speed's name in its colour, or the volume while setting it.
    bool vol = volume_mode_active(&s_vol_mode, now);
    int32_t volume_shown = vol ? s_volume : -1;
    if (volume_shown != shown_volume || speed_changed) {
        shown_volume = volume_shown;
        if (vol) {
            lv_obj_set_style_text_color(s_sub, COL_VOLUME, 0);
            lv_label_set_text_fmt(s_sub, "Volume %d", (int)s_volume);
        } else {
            const char *name = speed_name(st.speed, st.speed_count);
            lv_obj_set_style_text_color(s_sub, speed_colour(st.speed, st.speed_count), 0);
            if (name) {
                lv_label_set_text(s_sub, name);   // "" for a single-speed tune
            } else {
                lv_label_set_text_fmt(s_sub, "Speed %u", (unsigned)st.speed + 1u);
            }
        }
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
        title_check_fit(st.tune_count);
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
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);   // until the first reading, and while on USB
}

static void build_timeline(void)
{
    // A full-screen layer whose hit test only accepts the band round the edge (timeline_event LV_EVENT_HIT_TEST).
    s_tl = plain(s_scr);
    lv_obj_set_size(s_tl, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_center(s_tl);
    lv_obj_add_flag(s_tl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(s_tl, timeline_event, LV_EVENT_ALL, NULL);
    for (uint8_t i = 0; i < TUNE_INDEX_MAX_MARKERS; i++) {
        lv_obj_t *a = lv_arc_create(s_tl);
        lv_obj_set_size(a, BSP_LCD_H_RES - 8, BSP_LCD_V_RES - 8);   // where the old ring was: 170..176 px
        lv_obj_center(a);
        lv_arc_set_rotation(a, 270);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, TL_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, TL_WIDTH, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(a, LV_OPA_30, LV_PART_MAIN);   // unplayed: dim
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        s_tl_seg[i] = a;
    }
}

static void build_pause(void)
{
    s_pause = plain(s_scr);   // two rounded bars, shown while paused
    lv_obj_set_size(s_pause, 18, 20);
    lv_obj_align(s_pause, LV_ALIGN_CENTER, 0, PAUSE_Y);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = plain(s_pause);
        lv_obj_set_size(bar, 6, 20);
        lv_obj_set_pos(bar, (lv_coord_t)(i * 12), 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, COL_TEXT, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    lv_obj_add_flag(s_pause, LV_OBJ_FLAG_HIDDEN);
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

    // Pure black; the timeline ring round the edge.
    build_timeline();
    build_battery();
    build_pause();
#if UI_HAVE_ICONS
    s_icon = label(s_scr, UI_FONT_ICON, COL_ACCENT, ICON_Y);
#endif
    s_title = title_label(s_scr);
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
