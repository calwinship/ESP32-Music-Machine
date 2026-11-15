#include "logic/look.h"

#include <math.h>
#include <stdint.h>

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

void timeline_layout(const uint32_t *bounds, uint8_t count, int32_t gap_d10, int32_t *a0, int32_t *a1)
{
    if (count == 0) {
        return;
    }
    uint64_t total = bounds[count] > bounds[0] ? bounds[count] - bounds[0] : 0;
    int64_t room = 3600 - (int64_t)gap_d10 * count;
    if (room < 0) {
        room = 0;
    }
    for (uint8_t i = 0; i < count; i++) {
        // Cumulative, so rounding never adds up to a drift at the end.
        uint64_t from = bounds[i] - bounds[0], to = bounds[i + 1] - bounds[0];
        int64_t s = total ? (int64_t)(from * (uint64_t)room / total) : room * i / count;
        int64_t e = total ? (int64_t)(to * (uint64_t)room / total) : room * (i + 1) / count;
        a0[i] = (int32_t)(gap_d10 / 2 + (int64_t)gap_d10 * i + s);
        a1[i] = (int32_t)(gap_d10 / 2 + (int64_t)gap_d10 * i + e);
    }
}

int32_t timeline_played_d10(const uint32_t *bounds, uint8_t index, uint32_t position, const int32_t *a0,
                            const int32_t *a1)
{
    uint32_t b = bounds[index], e = bounds[index + 1];
    if (position <= b || e <= b) {
        return a0[index];
    }
    if (position >= e) {
        return a1[index];
    }
    return a0[index] + (int32_t)((int64_t)(a1[index] - a0[index]) * (position - b) / (e - b));
}

uint8_t timeline_segment_at(int32_t angle_d10, uint8_t count, const int32_t *a0, const int32_t *a1)
{
    if (count <= 1) {
        return 0;
    }
    int32_t a = ((angle_d10 % 3600) + 3600) % 3600;
    uint8_t best = 0;
    int32_t best_d = INT32_MAX;
    for (uint8_t i = 0; i < count; i++) {
        if (a >= a0[i] && a <= a1[i]) {
            return i;
        }
        // Distance round the circle to the nearer end of this segment.
        int32_t ends[2] = {a0[i], a1[i]};
        for (int k = 0; k < 2; k++) {
            int32_t d = a - ends[k];
            d = d < 0 ? -d : d;
            d = d > 1800 ? 3600 - d : d;
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
    }
    return best;
}

int32_t angle_d10(int32_t dx, int32_t dy)
{
    if (dx == 0 && dy == 0) {
        return 0;
    }
    double a = atan2((double)dx, (double)-dy) * 1800.0 / M_PI;
    return (int32_t)lround(a);
}

bool ring_hit(int32_t dx, int32_t dy, int32_t r_in, int32_t r_out)
{
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;
    return d2 >= (int64_t)r_in * r_in && d2 <= (int64_t)r_out * r_out;
}
