// Screen and input routing (LVGL). Call ui_start() once after bsp_display_start() and player_task_start().
//
//                    | main screen              | tune list open           | guide open
//   knob             | +/-2 s per click         | move to the next tune    | -
//   tap              | play/pause               | play the tapped tune     | close it
//   double-tap       | restart the current speed| -                        | -
//   speed scale      | tap a speed: play it from its start ("Slow - - - Fast" at the top)
//   list button      | open the tune list (three lines at the bottom)
//   swipe right      | nothing                  | back to the tune         | -
//   long-press       | volume mode (3 s): the knob sets volume 0-10
// Tune list: the centred tune is shown big (icon, title, length); the last row is "How to use" (the guide). It
// closes after a tap on a tune, a swipe right, or LIST_IDLE_MS without input; a tap that stops a moving list only
// stops it. The guide also shows by itself on the very first start (NVS player/guide_seen).
// Ring: progress through the current speed, coloured per speed (slow teal, medium amber, fast coral).
// Battery icon (top): level from logic/battery.h while on battery; a bolt while on USB.
// Boot: the backlight stays off until the first screen is drawn, then fades in (no white flash).
#pragma once
#include <stdint.h>

void ui_start(int32_t volume);
