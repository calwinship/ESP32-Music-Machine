// Control logic (plain C, no ESP-IDF headers; host-tested): knob seeking, volume steps, volume mode, click-free fades.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- knob seeking --------------------------------------------------------------------------------------------
#define SEEK_STEP_MS    2000   // one knob click
#define SEEK_SETTLE_MS  150    // clicks closer together than this are combined into one jump

typedef struct {
    int32_t pending;     // clicks not sent yet (+ forward, - back)
    uint32_t last_ms;    // time of the last click
} knob_seek_t;

void knob_seek_init(knob_seek_t *k);
void knob_seek_add(knob_seek_t *k, int32_t clicks, uint32_t now_ms);
// True (and *clicks = the combined count, pending cleared) once SEEK_SETTLE_MS have passed since the last click.
bool knob_seek_take(knob_seek_t *k, uint32_t now_ms, int32_t *clicks);

// New position after moving `clicks` x SEEK_STEP_MS from `position`, clamped to [0, total_samples - 1].
uint32_t seek_target(uint32_t position, int32_t clicks, uint32_t sample_rate, uint32_t total_samples);

// ---- volume --------------------------------------------------------------------------------------------------
#define VOLUME_MIN     0      // silent
#define VOLUME_MAX     10     // full line level
#define VOLUME_DEFAULT 6      // -12 dB
#define VOLUME_MODE_MS 3000   // volume mode ends this long after entering it or the last knob click

// Gain for a volume step, in Q15 (32767 = 1.0): 3 dB per step below VOLUME_MAX, 0 for VOLUME_MIN.
// Step 3 (-21 dB) is about the fixed 0.1 used before volume control existed.
uint16_t volume_gain_q15(int32_t step);
int32_t volume_apply(int32_t step, int32_t delta);   // clamped to [VOLUME_MIN, VOLUME_MAX]
bool volume_valid(int32_t step);

typedef struct {
    bool active;
    uint32_t until_ms;
} volume_mode_t;

void volume_mode_enter(volume_mode_t *m, uint32_t now_ms);
void volume_mode_touch(volume_mode_t *m, uint32_t now_ms);   // a knob click in volume mode: extend it
bool volume_mode_active(volume_mode_t *m, uint32_t now_ms);  // also ends it once it has timed out

// ---- click-free transitions ----------------------------------------------------------------------------------
#define FADE_SAMPLES 160   // 5 ms at 32 kHz

// Fill out[0..n) with a straight ramp from `from` down to 0 (out[n-1] == 0). Written after the last sample before
// a jump or pause, so the waveform doesn't step from mid-signal to silence.
void fade_ramp_to_zero(int16_t from, int16_t *out, uint32_t n);

// Fade-in over FADE_SAMPLES after a jump/resume: scales buf in place. *done counts samples already faded
// (start at 0); once it reaches FADE_SAMPLES the samples pass unchanged.
void fade_in(int16_t *buf, uint32_t n, uint32_t *done);

#ifdef __cplusplus
}
#endif
