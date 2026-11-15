// Screen geometry and colour decisions that need no hardware (host-tested: tests/host/test_look.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPEED_RGB_SLOW   0x2EC4B6u   // teal
#define SPEED_RGB_MEDIUM 0xF2A33Au   // amber
#define SPEED_RGB_FAST   0xFF5A5Fu   // coral

// Colour of speed `index` of `count` (0xRRGGBB): slowest teal, middle amber, fastest coral, blended in between.
// 1 speed: amber; 2: teal, coral; 3: teal, amber, coral exactly.
uint32_t speed_rgb(uint8_t index, uint8_t count);

// Speed scale: an arc around the top of the screen, concentric with the perimeter ring. Positions along it are in
// tenths of a degree (0 = straight up, clockwise positive); "track" coordinates run 0..track_w from its left end.
// `count` equal segments, `gap` apart.
void scale_segment(uint8_t index, uint8_t count, int32_t track_w, int32_t gap, int32_t *x, int32_t *w);
// Where the position dot goes: `permille` of the way through segment `index`.
int32_t scale_dot_x(uint8_t index, uint16_t permille, uint8_t count, int32_t track_w, int32_t gap);
// Which speed a tap at x (track coordinates) means; before the track = the first, after it = the last.
uint8_t scale_speed_at(int32_t x, uint8_t count, int32_t track_w);

// Angle of the point (dx, dy) from the screen centre (y down), tenths of a degree: 0 up, +900 right, -900 left.
int32_t angle_d10(int32_t dx, int32_t dy);
// Is (dx, dy) on the scale's touch band: within `band` px of radius `r` and within +/-`half_d10` of straight up?
bool scale_hit(int32_t dx, int32_t dy, int32_t r, int32_t band, int32_t half_d10);

#ifdef __cplusplus
}
#endif
