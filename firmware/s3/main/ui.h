// Screen and input routing (LVGL). Call ui_start() once after bsp_display_start() and player_task_start().
//
//                    | main screen              | tune list open           | volume mode (3 s)
//   knob             | +/-2 s per click         | move to the next tune    | volume 0-10
//   tap              | play/pause               | play the tapped tune     | play/pause
//   double-tap       | restart the current speed| -                        | -
//   speed button     | play that speed from its start (Slow / Medium / Fast across the top)
//   list button      | open the tune list (three lines at the bottom)
//   swipe left/right | nothing                  | close the list           | -
//   long-press       | volume mode              | -                        | -
// The list closes after a tap on a tune, a sideways swipe, or LIST_IDLE_MS without input. A tap that stops a moving
// list only stops it.
// Ring: progress through the current speed, coloured per speed (slow teal, medium amber, fast coral).
// Battery icon (top): level from logic/battery.h while on battery; a bolt while on USB.
// Boot: the backlight stays off until the first tune's screen is drawn, then fades in (no white flash).
#pragma once
#include <stdint.h>

void ui_start(int32_t volume);
