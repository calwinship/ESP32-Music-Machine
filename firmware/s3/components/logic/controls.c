#include "logic/controls.h"

// ---- knob seeking --------------------------------------------------------------------------------------------
void knob_seek_init(knob_seek_t *k)
{
    k->pending = 0;
    k->last_ms = 0;
}

void knob_seek_add(knob_seek_t *k, int32_t clicks, uint32_t now_ms)
{
    if (clicks) {
        k->pending += clicks;
        k->last_ms = now_ms;
    }
}

bool knob_seek_take(knob_seek_t *k, uint32_t now_ms, int32_t *clicks)
{
    if (k->pending == 0 || (uint32_t)(now_ms - k->last_ms) < SEEK_SETTLE_MS) {
        return false;
    }
    *clicks = k->pending;
    k->pending = 0;
    return true;
}

uint32_t seek_target(uint32_t position, int32_t clicks, uint32_t sample_rate, uint32_t total_samples)
{
    if (total_samples == 0) {
        return 0;
    }
    int64_t t = (int64_t)position + (int64_t)clicks * SEEK_STEP_MS * (int64_t)sample_rate / 1000;
    if (t < 0) {
        t = 0;
    }
    if (t > (int64_t)total_samples - 1) {
        t = (int64_t)total_samples - 1;
    }
    return (uint32_t)t;
}

// ---- volume --------------------------------------------------------------------------------------------------
// round(32767 * 10^(-3 * (10 - step) / 20)) for steps 1..10; step 0 is silence.
static const uint16_t k_gain_q15[VOLUME_MAX + 1] = {0, 1464, 2068, 2920, 4125, 5827, 8231, 11627, 16423, 23197,
                                                    32767};

bool volume_valid(int32_t step)
{
    return step >= VOLUME_MIN && step <= VOLUME_MAX;
}

uint16_t volume_gain_q15(int32_t step)
{
    return k_gain_q15[volume_apply(step, 0)];
}

int32_t volume_apply(int32_t step, int32_t delta)
{
    int64_t v = (int64_t)step + delta;
    return v < VOLUME_MIN ? VOLUME_MIN : v > VOLUME_MAX ? VOLUME_MAX : (int32_t)v;
}

void volume_mode_enter(volume_mode_t *m, uint32_t now_ms)
{
    m->active = true;
    m->until_ms = now_ms + VOLUME_MODE_MS;
}

void volume_mode_touch(volume_mode_t *m, uint32_t now_ms)
{
    if (m->active) {
        m->until_ms = now_ms + VOLUME_MODE_MS;
    }
}

bool volume_mode_active(volume_mode_t *m, uint32_t now_ms)
{
    if (m->active && (int32_t)(now_ms - m->until_ms) >= 0) {   // wrap-safe
        m->active = false;
    }
    return m->active;
}

// ---- click-free transitions ----------------------------------------------------------------------------------
void fade_ramp_to_zero(int16_t from, int16_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (int16_t)((int32_t)from * (int32_t)(n - 1 - i) / (int32_t)(n > 1 ? n - 1 : 1));
    }
}

void fade_in(int16_t *buf, uint32_t n, uint32_t *done)
{
    for (uint32_t i = 0; i < n && *done < FADE_SAMPLES; i++, (*done)++) {
        buf[i] = (int16_t)((int32_t)buf[i] * (int32_t)*done / FADE_SAMPLES);
    }
}
