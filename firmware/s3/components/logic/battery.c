#include "logic/battery.h"

#include <string.h>

// Resting lithium-ion cell, roughly; "empty" moved up to where the board's 3.3 V buck regulator runs out of headroom.
static const struct {
    uint16_t mv;
    uint8_t level;
} k_curve[] = {
    {BATTERY_EMPTY_MV, 0}, {3550, 4}, {3650, 10}, {3720, 20}, {3780, 35},
    {3850, 52}, {3950, 70}, {4050, 85}, {BATTERY_FULL_MV, 100},
};
#define CURVE_N (sizeof(k_curve) / sizeof(k_curve[0]))

uint8_t battery_level_from_mv(uint32_t mv)
{
    if (mv <= k_curve[0].mv) {
        return 0;
    }
    for (unsigned i = 1; i < CURVE_N; i++) {
        if (mv <= k_curve[i].mv) {
            uint32_t v0 = k_curve[i - 1].mv, v1 = k_curve[i].mv;
            uint32_t l0 = k_curve[i - 1].level, l1 = k_curve[i].level;
            return (uint8_t)(l0 + (l1 - l0) * (mv - v0) / (v1 - v0));
        }
    }
    return 100;
}

void battery_init(battery_t *b)
{
    memset(b, 0, sizeof(*b));
}

battery_state_t battery_update(battery_t *b, uint32_t mv)
{
    if (mv == 0) {
        return b->state;
    }
    bool plugged = mv >= BATTERY_USB_MV;
    if (!b->state.known || plugged != b->state.plugged) {
        // First reading, or plugged in / unplugged: start the average afresh (the USB rail is not the cell).
        b->avg_x16 = mv * BATTERY_SMOOTH;
        b->state.known = true;
        b->state.plugged = plugged;
        b->state.level = plugged ? 0 : battery_level_from_mv(mv);
        return b->state;
    }
    b->avg_x16 = b->avg_x16 - b->avg_x16 / BATTERY_SMOOTH + mv;
    if (!plugged) {
        uint8_t now = battery_level_from_mv(b->avg_x16 / BATTERY_SMOOTH);
        if (now + BATTERY_DROP_STEP <= b->state.level || now >= b->state.level + BATTERY_RISE_STEP || now == 0) {
            b->state.level = now;
        }
    }
    return b->state;
}
