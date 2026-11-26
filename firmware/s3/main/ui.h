// Screen and input routing (LVGL). Call ui_start() once after bsp_display_start() and player_task_start().
//
//                    | main screen              | tune list open           | guide open
//   knob             | +/-2 s per click         | move to the next tune    | -
//   tap              | play/pause               | play the tapped tune     | close it
//   double-tap       | restart the current speed| -                        | -
//   ring (edge)      | tap a speed's segment: play that speed from its start
//   list button      | open the tune list (three lines at the bottom)
//   swipe right      | nothing                  | back to the tune         | -
//   long-press       | volume mode (3 s): the knob sets volume 0-10
// Tune list: the centred tune is shown big (icon, title, length); the last row is "How to use" (the guide). It
// closes after a tap on a tune, a swipe right, or LIST_IDLE_MS without input; a tap that stops a moving list only
// stops it. The guide also shows by itself on the very first start (NVS player/guide_seen).
// Ring: the whole tune, one segment per speed (slow teal, medium amber, fast coral), played part bright.
// Under the time: the current speed's name in its colour. Pause bars at the top while paused.
// Background pure black; the tune's colour is only on its icon.
// Battery icon (top): level from logic/battery.h, only while on battery (hidden on USB).
// Boot: the backlight stays off until the first screen is drawn, then fades in (no white flash).
#pragma once
#include <stdint.h>

void ui_start(int32_t volume);
// Sync mode (instead of ui_start): "Syncing tunes", a percentage (ui_sync_progress; < 0 hides it) and a hint.
void ui_sync_start(void);
void ui_sync_progress(int percent);
