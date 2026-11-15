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

// Timeline ring: the whole tune once round the edge, starting at the top, one segment per speed with its length in
// proportion to the speed's duration and `gap_d10` between segments (also between the last and the first).
// Angles are tenths of a degree clockwise from straight up, 0..3600.
// bounds: count + 1 times (any unit, increasing): speed i runs from bounds[i] to bounds[i + 1].
void timeline_layout(const uint32_t *bounds, uint8_t count, int32_t gap_d10, int32_t *a0, int32_t *a1);
// How far round segment `index` the played part reaches for `position` (same unit as bounds): a0[index]..a1[index].
int32_t timeline_played_d10(const uint32_t *bounds, uint8_t index, uint32_t position, const int32_t *a0,
                            const int32_t *a1);
// Which segment a tap at `angle` (tenths of a degree, any value) means: the one it is on, or the nearest one if
// it is in a gap.
uint8_t timeline_segment_at(int32_t angle_d10, uint8_t count, const int32_t *a0, const int32_t *a1);

// Angle of the point (dx, dy) from the screen centre (y down), tenths of a degree: 0 up, +900 right, -900 left.
int32_t angle_d10(int32_t dx, int32_t dy);
// Is (dx, dy) between radius r_in and r_out?
bool ring_hit(int32_t dx, int32_t dy, int32_t r_in, int32_t r_out);

#ifdef __cplusplus
}
#endif
