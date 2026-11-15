// Host tests for logic/controls and player_seek (built and run by ./test).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "logic/controls.h"
#include "logic/playback.h"

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

#define SR 32000u

static void test_quick_clicks_combine_into_one_jump(void)
{
    knob_seek_t k;
    knob_seek_init(&k);
    int32_t c = 99;
    CHECK(!knob_seek_take(&k, 1000, &c));          // nothing pending
    knob_seek_add(&k, -1, 1000);
    knob_seek_add(&k, -1, 1060);
    knob_seek_add(&k, -1, 1120);
    CHECK(!knob_seek_take(&k, 1200, &c));          // 80 ms after the last click: still turning
    CHECK(knob_seek_take(&k, 1270, &c));           // 150 ms of quiet: one jump of -3 clicks
    CHECK_EQ(c, -3);
    CHECK(!knob_seek_take(&k, 5000, &c));          // and only once
    knob_seek_add(&k, 2, 6000);
    knob_seek_add(&k, -2, 6050);                   // back and forth cancels out
    CHECK(!knob_seek_take(&k, 9000, &c));
    knob_seek_add(&k, 0, 9000);                    // a zero delta doesn't restart the wait
    CHECK(!knob_seek_take(&k, 9000, &c));
}

static void test_click_timer_survives_wrap(void)
{
    knob_seek_t k;
    knob_seek_init(&k);
    int32_t c;
    knob_seek_add(&k, 1, UINT32_MAX - 50);
    CHECK(!knob_seek_take(&k, 40, &c));            // 91 ms later, across the wrap
    CHECK(knob_seek_take(&k, 120, &c));
    CHECK_EQ(c, 1);
}

static void test_seek_target(void)
{
    uint32_t total = 203 * SR;
    CHECK_EQ(seek_target(60 * SR, -1, SR, total), 58 * SR);   // one click back = 2 s
    CHECK_EQ(seek_target(60 * SR, 3, SR, total), 66 * SR);
    CHECK_EQ(seek_target(SR, -5, SR, total), 0);              // stops at the start
    CHECK_EQ(seek_target(200 * SR, 10, SR, total), total - 1);  // and at the last sample
    CHECK_EQ(seek_target(0, INT32_MIN, SR, total), 0);        // no overflow
    CHECK_EQ(seek_target(0, INT32_MAX, SR, total), total - 1);
    CHECK_EQ(seek_target(5, 1, SR, 0), 0);
}

static void test_volume_steps(void)
{
    CHECK_EQ(volume_gain_q15(VOLUME_MIN), 0);
    CHECK_EQ(volume_gain_q15(VOLUME_MAX), 32767);
    CHECK_EQ(volume_gain_q15(VOLUME_DEFAULT), 8231);           // -12 dB
    CHECK_EQ(volume_gain_q15(3), 2920);                       // -21 dB ~ the old fixed 0.1 (3277)
    for (int32_t s = VOLUME_MIN + 1; s <= VOLUME_MAX; s++) {
        CHECK(volume_gain_q15(s) > volume_gain_q15(s - 1));
        // each step is 3 dB: ratio 1.413 +- 1 %
        if (s > 1) {
            double r = (double)volume_gain_q15(s) / volume_gain_q15(s - 1);
            CHECK(r > 1.40 && r < 1.427);
        }
    }
    CHECK_EQ(volume_gain_q15(-4), 0);                         // out of range clamps
    CHECK_EQ(volume_gain_q15(99), 32767);
    CHECK_EQ(volume_apply(9, 3), VOLUME_MAX);
    CHECK_EQ(volume_apply(1, -3), VOLUME_MIN);
    CHECK_EQ(volume_apply(5, -1), 4);
    CHECK(volume_valid(0) && volume_valid(10) && !volume_valid(11) && !volume_valid(-1));
}

static void test_volume_mode_times_out(void)
{
    volume_mode_t m = {0};
    CHECK(!volume_mode_active(&m, 0));
    volume_mode_touch(&m, 0);                                 // touching when inactive does nothing
    CHECK(!volume_mode_active(&m, 0));
    volume_mode_enter(&m, 1000);
    CHECK(volume_mode_active(&m, 3999));
    volume_mode_touch(&m, 3000);                              // a click extends it to 6000
    CHECK(volume_mode_active(&m, 5999));
    CHECK(!volume_mode_active(&m, 6000));
    CHECK(!volume_mode_active(&m, 6001));
    volume_mode_enter(&m, UINT32_MAX - 1000);                 // across the timer wrap
    CHECK(volume_mode_active(&m, 500));
    CHECK(!volume_mode_active(&m, 2000));
}

static void test_fades(void)
{
    int16_t ramp[FADE_SAMPLES];
    fade_ramp_to_zero(-20000, ramp, FADE_SAMPLES);
    CHECK_EQ(ramp[0], -20000);
    CHECK_EQ(ramp[FADE_SAMPLES - 1], 0);
    int ok = 1;
    for (int i = 1; i < FADE_SAMPLES; i++) {
        ok &= ramp[i] >= ramp[i - 1] && ramp[i] - ramp[i - 1] <= 20000 / (FADE_SAMPLES - 1) + 1;
    }
    CHECK(ok);
    int16_t one;
    fade_ramp_to_zero(1234, &one, 1);
    CHECK_EQ(one, 0);

    int16_t buf[400];
    for (int i = 0; i < 400; i++) {
        buf[i] = 30000;
    }
    uint32_t done = 0;
    fade_in(buf, 100, &done);                                 // fade spans two writes
    fade_in(buf + 100, 300, &done);
    CHECK_EQ(done, FADE_SAMPLES);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[80], 15000);
    CHECK(buf[FADE_SAMPLES - 1] < 30000);
    CHECK_EQ(buf[FADE_SAMPLES], 30000);                       // untouched after the fade
    CHECK_EQ(buf[399], 30000);
}

static void test_player_seek(void)
{
    player_t p;
    player_init(&p);
    CHECK(!player_seek(&p, 100));                             // idle: no effect
    player_start(&p, SR, 10 * SR, 0);
    for (int i = 0; i < 100; i++) {
        player_accept(&p, 1152);
    }
    CHECK(player_seek(&p, 2 * SR));
    CHECK_EQ(p.position, 2 * SR);
    CHECK_EQ(p.state, PLAYER_PLAYING);
    CHECK(player_log_due(&p));                                // the log follows the jump
    CHECK(!player_log_due(&p));
    player_toggle(&p);
    CHECK(player_seek(&p, 99 * SR));                          // clamps; paused stays paused
    CHECK_EQ(p.position, 10 * SR - 1);
    CHECK_EQ(p.state, PLAYER_PAUSED);
    player_toggle(&p);
    player_accept(&p, 1152);
    CHECK_EQ(p.state, PLAYER_ENDED);
    CHECK(player_seek(&p, 4 * SR));                           // after the end: paused at the new place
    CHECK_EQ(p.state, PLAYER_PAUSED);
    CHECK_EQ(player_elapsed_ms(&p), 4000);
    player_fail(&p);
    CHECK(!player_seek(&p, 0));
}

int main(void)
{
    test_quick_clicks_combine_into_one_jump();
    test_click_timer_survives_wrap();
    test_seek_target();
    test_volume_steps();
    test_volume_mode_times_out();
    test_fades();
    test_player_seek();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
