#include "logic/knob.h"

void knob_decoder_init(knob_decoder_t *d)
{
    // Lines idle high (10 kΩ pull-ups R59/R60 + internal pull-ups).
    d->a.prev_level = 1;
    d->a.low_count = 0;
    d->b = d->a;
}

// Same decisions as the vendor's process_knob_channel(), except that low_count saturates.
static bool channel_step(knob_channel_t *c, uint8_t level)
{
    bool fired = false;
    if (level == 0) {
        if (level != c->prev_level) {
            c->low_count = 0;
        } else if (c->low_count < UINT8_MAX) {
            c->low_count++;
        }
    } else {
        // Vendor: `++cnt >= DEBOUNCE`. Compare without the increment so a saturated count can't wrap to 0.
        if (level != c->prev_level && (unsigned)c->low_count + 1u >= KNOB_DEBOUNCE_SAMPLES) {
            fired = true;
        }
        c->low_count = 0;
    }
    c->prev_level = level;
    return fired;
}

int knob_decoder_step(knob_decoder_t *d, int level_a, int level_b)
{
    int delta = 0;
    if (channel_step(&d->a, level_a ? 1 : 0)) {
        delta += 1;
    }
    if (channel_step(&d->b, level_b ? 1 : 0)) {
        delta -= 1;
    }
    return delta;
}

static int32_t clamp(int32_t x, int32_t lo, int32_t hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

void knob_value_init(knob_value_t *v, int32_t min, int32_t max, int32_t initial)
{
    v->min = min;
    v->max = max;
    v->value = clamp(initial, min, max);
}

bool knob_value_set(knob_value_t *v, int32_t value)
{
    int32_t nv = clamp(value, v->min, v->max);
    bool changed = nv != v->value;
    v->value = nv;
    return changed;
}

bool knob_value_apply(knob_value_t *v, int32_t delta)
{
    // Add in 64-bit so value + delta cannot overflow int32, then clamp.
    int64_t target = (int64_t)v->value + delta;
    if (target < v->min) {
        target = v->min;
    }
    if (target > v->max) {
        target = v->max;
    }
    return knob_value_set(v, (int32_t)target);
}
