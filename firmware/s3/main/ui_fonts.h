// UI fonts. With the generated Latin-1 fonts (main/fonts/, made by tools/gen_fonts.sh) titles show Irish fadas;
// without them the build falls back to LVGL's built-in ASCII Montserrat and folds accents (Nóra -> Nora).
// main/CMakeLists.txt defines UI_LATIN1_FONTS when main/fonts/font_*.c exist.
#pragma once
#include "lvgl.h"

#if UI_LATIN1_FONTS
LV_FONT_DECLARE(font_text_20)
LV_FONT_DECLARE(font_text_14)
LV_FONT_DECLARE(font_time_64)
LV_FONT_DECLARE(font_icon_56)
#define UI_FONT_ICON         (&font_icon_56)   // tune icons (tools/icons.txt)
#define UI_HAVE_ICONS        1
#define UI_FONT_TEXT         (&font_text_20)   // titles, list, play/pause symbol
#define UI_FONT_SMALL        (&font_text_14)
#define UI_FONT_TIME         (&font_time_64)
#define UI_FOLD_ACCENTS      0
#else
#define UI_FONT_TEXT         (&lv_font_montserrat_20)
#define UI_FONT_SMALL        (&lv_font_montserrat_14)
#define UI_FONT_TIME         (&lv_font_montserrat_48)
#define UI_FOLD_ACCENTS      1
#define UI_HAVE_ICONS        0                 // no icons without the generated fonts
#endif
