#include "logic/look.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static uint32_t blend(uint32_t a, uint32_t b, uint32_t num, uint32_t den)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        uint32_t c = (ca * (den - num) + cb * num + den / 2) / den;
        out |= c << shift;
    }
    return out;
}

uint32_t speed_rgb(uint8_t index, uint8_t count)
{
    if (count <= 1) {
        return SPEED_RGB_MEDIUM;
    }
    if (index >= count) {
        index = (uint8_t)(count - 1);
    }
    // Position along slow..fast as a fraction t = index / (count - 1), in units of 1/(2*(count-1)) so the
    // teal->amber half and the amber->coral half are both exact.
    uint32_t den = 2u * (count - 1u), t = 2u * index;
    if (t <= den / 2) {
        return blend(SPEED_RGB_SLOW, SPEED_RGB_MEDIUM, t, den / 2);
    }
    return blend(SPEED_RGB_MEDIUM, SPEED_RGB_FAST, t - den / 2, den / 2);
}

void scale_segment(uint8_t index, uint8_t count, int32_t track_w, int32_t gap, int32_t *x, int32_t *w)
{
    if (count == 0) {
        *x = 0;
        *w = track_w;
        return;
    }
    int32_t seg = (track_w - gap * (count - 1)) / count;
    if (index >= count) {
        index = (uint8_t)(count - 1);
    }
    *x = index * (seg + gap);
    *w = seg;
}

int32_t scale_dot_x(uint8_t index, uint16_t permille, uint8_t count, int32_t track_w, int32_t gap)
{
    int32_t x, w;
    scale_segment(index, count, track_w, gap, &x, &w);
    if (permille > 1000) {
        permille = 1000;
    }
    return x + (int32_t)((int64_t)w * permille / 1000);
}

uint8_t scale_speed_at(int32_t x, uint8_t count, int32_t track_w)
{
    if (count <= 1 || x < 0 || track_w <= 0) {
        return 0;
    }
    if (x >= track_w) {
        return (uint8_t)(count - 1);
    }
    return (uint8_t)((int64_t)x * count / track_w);
}

int32_t angle_d10(int32_t dx, int32_t dy)
{
    if (dx == 0 && dy == 0) {
        return 0;
    }
    double a = atan2((double)dx, (double)-dy) * 1800.0 / M_PI;
    return (int32_t)lround(a);
}

bool scale_hit(int32_t dx, int32_t dy, int32_t r, int32_t band, int32_t half_d10)
{
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy, lo = r - band, hi = (int64_t)r + band;
    if (lo < 0) {
        lo = 0;
    }
    if (d2 < lo * lo || d2 > hi * hi) {
        return false;
    }
    int32_t a = angle_d10(dx, dy);
    return a >= -half_d10 && a <= half_d10;
}
