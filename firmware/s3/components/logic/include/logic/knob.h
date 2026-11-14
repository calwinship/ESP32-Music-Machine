// Hardware-independent knob logic (plain C, no ESP-IDF headers): unit-tested on the host, see tests/host/.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The board's encoder (SSCM110100, docs/hardware.md) is a "bidirectional switch", not a quadrature encoder:
// each detent pulses ONE line low, A for clockwise and B for anticlockwise. Clockwise = A is confirmed on hardware:
// the vendor's A event raised the volume, and the user heard it get louder turning clockwise.
//
// The decoder is a port of the vendor's bidi_switch_knob.c process_knob_channel(), sampled every 3 ms. A step is
// counted on the rising edge (low -> high) of a line that was low for at least KNOB_DEBOUNCE_SAMPLES samples.
// One deliberate difference: the low-sample counter saturates instead of wrapping at 256, so a line held low for
// more than 255 samples (~0.8 s) still counts its step on release.
#define KNOB_DEBOUNCE_SAMPLES 2

typedef struct {
    uint8_t prev_level;  // 0 = low (active), 1 = high (idle, pulled up)
    uint8_t low_count;
} knob_channel_t;

typedef struct {
    knob_channel_t a;  // clockwise line
    knob_channel_t b;  // anticlockwise line
} knob_decoder_t;

void knob_decoder_init(knob_decoder_t *d);

// Feed one sample of both lines (0 = low, non-zero = high).
// Returns +1 (clockwise step), -1 (anticlockwise step), or 0 (no step, or both lines in the same sample).
int knob_decoder_step(knob_decoder_t *d, int level_a, int level_b);

// A value driven by knob steps, clamped to [min, max].
typedef struct {
    int32_t value;
    int32_t min;
    int32_t max;
} knob_value_t;

void knob_value_init(knob_value_t *v, int32_t min, int32_t max, int32_t initial);
// Add delta (clamped). Returns true if the value changed.
bool knob_value_apply(knob_value_t *v, int32_t delta);
// Set an absolute value (clamped). Returns true if the value changed.
bool knob_value_set(knob_value_t *v, int32_t value);

#ifdef __cplusplus
}
#endif
