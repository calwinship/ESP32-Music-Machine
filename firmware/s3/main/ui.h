// Screen and input routing (LVGL). Call ui_start() once after bsp_display_start() and player_task_start().
//
//                    | main screen              | tune list open           | volume mode (3 s)
//   knob             | +/-2 s per click         | move the highlight       | volume 0-10
//   tap              | play/pause               | play the highlighted tune| play/pause
//   double-tap       | restart the current speed| -                        | -
//   swipe left/right | next / previous speed    | -                        | -
//   list button      | open the tune list       | -                        | -
//   long-press       | volume mode              | -                        | -
// The list closes after a tap on a tune or LIST_IDLE_MS without input; dragging it only scrolls.
// Ring: progress through the current speed, coloured per speed (slow teal, medium amber, fast coral).
#pragma once
#include <stdint.h>

void ui_start(int32_t volume);
