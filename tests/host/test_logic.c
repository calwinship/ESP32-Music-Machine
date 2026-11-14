// Host unit tests for firmware/s3/components/logic (plain C; built and run by ./test).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "logic/cst816.h"
#include "logic/knob.h"

static int g_checks, g_failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        long long _a = (long long)(a), _b = (long long)(b);                      \
        g_checks++;                                                              \
        if (_a != _b) {                                                          \
            g_failures++;                                                        \
            printf("FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, \
                   #a, _a, _b);                                                  \
        }                                                                        \
    } while (0)

// Feed n identical samples, return the summed delta.
static int feed(knob_decoder_t *d, int a, int b, int n)
{
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += knob_decoder_step(d, a, b);
    }
    return sum;
}

static void test_idle_produces_nothing(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    CHECK_EQ(feed(&d, 1, 1, 1000), 0);
}

static void test_clockwise_detent(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    CHECK_EQ(feed(&d, 0, 1, 2), 0);           // A low: nothing until release
    CHECK_EQ(knob_decoder_step(&d, 1, 1), 1); // rising edge after >= 2 low samples
    CHECK_EQ(feed(&d, 1, 1, 10), 0);
}

static void test_anticlockwise_detent(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    CHECK_EQ(feed(&d, 1, 0, 5), 0);
    CHECK_EQ(knob_decoder_step(&d, 1, 1), -1);
}

static void test_single_sample_glitch_rejected(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    CHECK_EQ(knob_decoder_step(&d, 0, 1), 0);
    CHECK_EQ(knob_decoder_step(&d, 1, 1), 0); // low for only 1 sample: bounce, no step
}

static void test_bouncing_contact_counts_once(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    // Contact chatter on press, then a solid low, then release.
    int seq[] = {0, 1, 0, 1, 0, 0, 0, 0, 1};
    int sum = 0;
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        sum += knob_decoder_step(&d, seq[i], 1);
    }
    CHECK_EQ(sum, 1);
}

static void test_both_lines_same_sample_cancel(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    feed(&d, 0, 0, 3);
    CHECK_EQ(knob_decoder_step(&d, 1, 1), 0);
}

static void test_many_detents(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    int sum = 0;
    for (int i = 0; i < 7; i++) {
        sum += feed(&d, 0, 1, 3) + feed(&d, 1, 1, 5);
    }
    for (int i = 0; i < 3; i++) {
        sum += feed(&d, 1, 0, 3) + feed(&d, 1, 1, 5);
    }
    CHECK_EQ(sum, 7 - 3);
}

static void test_long_hold_still_counts(void)
{
    // Vendor's uint8 counter would wrap; ours saturates. 256*k + 1 low samples is a wrap point for the vendor code.
    knob_decoder_t d;
    knob_decoder_init(&d);
    CHECK_EQ(feed(&d, 0, 1, 257), 0);
    CHECK_EQ(knob_decoder_step(&d, 1, 1), 1);
}

static void test_value_clamps(void)
{
    knob_value_t v;
    knob_value_init(&v, 0, 100, 150);
    CHECK_EQ(v.value, 100); // initial clamped
    CHECK(!knob_value_apply(&v, 5));
    CHECK(knob_value_apply(&v, -3));
    CHECK_EQ(v.value, 97);
    CHECK(knob_value_apply(&v, -1000));
    CHECK_EQ(v.value, 0);
    CHECK(!knob_value_apply(&v, -1));
    CHECK(knob_value_set(&v, 50));
    CHECK(!knob_value_set(&v, 50));
    CHECK(knob_value_set(&v, 999));
    CHECK_EQ(v.value, 100);
    knob_value_init(&v, INT32_MIN, INT32_MAX, INT32_MAX);
    CHECK(!knob_value_apply(&v, INT32_MAX)); // no overflow
    CHECK_EQ(v.value, INT32_MAX);
}

// --- Equivalence with the vendor implementation -------------------------------------------------------------
// Verbatim logic of process_knob_channel() from the vendor's
// ESP-IDF/04_Encoder_Test/components/user_encoder_bsp/src/bidi_switch_knob.c (lines 63-87), minus the callback.
static int vendor_channel(uint8_t current_level, uint8_t *prev_level, uint8_t *debounce_cnt)
{
    int fired = 0;
    if (current_level == 0) {
        if (current_level != *prev_level)
            *debounce_cnt = 0;
        else
            (*debounce_cnt)++;
    } else {
        if (current_level != *prev_level && ++(*debounce_cnt) >= KNOB_DEBOUNCE_SAMPLES) {
            *debounce_cnt = 0;
            fired = 1;
        } else
            *debounce_cnt = 0;
    }
    *prev_level = current_level;
    return fired;
}

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_equivalent_to_vendor(void)
{
    knob_decoder_t d;
    knob_decoder_init(&d);
    // The vendor starts from calloc (prev 0); ours starts at idle (prev 1). Align by one idle sample each.
    uint8_t pa = 0, ca = 0, pb = 0, cb = 0;
    vendor_channel(1, &pa, &ca);
    vendor_channel(1, &pb, &cb);
    knob_decoder_step(&d, 1, 1);

    int mismatches = 0;
    int a = 1, b = 1, run_a = 0, run_b = 0;
    for (int i = 0; i < 200000; i++) {
        // Random runs of 1..20 samples per level (never long enough to hit the vendor's uint8 wrap).
        if (run_a-- <= 0) {
            a = !a;
            run_a = 1 + (int)(rng() % 20);
        }
        if (run_b-- <= 0) {
            b = !b;
            run_b = 1 + (int)(rng() % 20);
        }
        int expect = vendor_channel((uint8_t)a, &pa, &ca) - vendor_channel((uint8_t)b, &pb, &cb);
        int got = knob_decoder_step(&d, a, b);
        if (expect != got) {
            mismatches++;
        }
    }
    CHECK_EQ(mismatches, 0);
}

// --- CST816 ---------------------------------------------------------------------------------------------------
static void test_cst816_no_touch(void)
{
    uint8_t r[CST816_REPORT_LEN] = {0, 0, 0, 0x01, 0x23, 0x01, 0x45};
    cst816_point_t p = cst816_parse(r, 360, 360);
    CHECK(!p.pressed);
}

static void test_cst816_touch(void)
{
    // x = 0x123 = 291, y = 0x045 = 69; high nibble of [3]/[5] is event flags and must be masked off.
    uint8_t r[CST816_REPORT_LEN] = {0, 0, 1, 0x81, 0x23, 0x40, 0x45};
    cst816_point_t p = cst816_parse(r, 360, 360);
    CHECK(p.pressed);
    CHECK_EQ(p.x, 291);
    CHECK_EQ(p.y, 69);
}

static void test_cst816_clamps_to_last_pixel(void)
{
    uint8_t r[CST816_REPORT_LEN] = {0, 0, 1, 0x01, 0x68, 0x0F, 0xFF}; // x = 360, y = 4095
    cst816_point_t p = cst816_parse(r, 360, 360);
    CHECK_EQ(p.x, 359);
    CHECK_EQ(p.y, 359);
}

int main(void)
{
    test_idle_produces_nothing();
    test_clockwise_detent();
    test_anticlockwise_detent();
    test_single_sample_glitch_rejected();
    test_bouncing_contact_counts_once();
    test_both_lines_same_sample_cancel();
    test_many_detents();
    test_long_hold_still_counts();
    test_value_clamps();
    test_equivalent_to_vendor();
    test_cst816_no_touch();
    test_cst816_touch();
    test_cst816_clamps_to_last_pixel();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
