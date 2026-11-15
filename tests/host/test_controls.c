// Host tests for logic/controls and player_seek (built and run by ./test).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static tune_info_t three_speeds(void)
{
    tune_info_t t = {.sample_rate = SR, .total_samples = 203 * SR, .samples_per_frame = 1152, .marker_count = 3,
                     .markers_ms = {6800, 101000, 157800}};
    return t;
}

static void test_speeds(void)
{
    tune_info_t t = three_speeds();
    speed_t s;
    speed_at(&t, 0, &s);                                       // lead-in counts as speed 0, progress 0
    CHECK_EQ(s.index, 0);
    CHECK_EQ(s.count, 3);
    CHECK_EQ(s.start, 6800 * 32);
    CHECK_EQ(s.end, 101000 * 32);
    CHECK_EQ(speed_permille(&s, 0), 0);
    speed_at(&t, 101000 * 32 - 1, &s);
    CHECK_EQ(s.index, 0);
    CHECK(speed_permille(&s, 101000 * 32 - 1) >= 999);
    speed_at(&t, 101000 * 32, &s);                             // exactly on a marker: the new speed
    CHECK_EQ(s.index, 1);
    CHECK_EQ(speed_permille(&s, 101000 * 32), 0);
    speed_at(&t, 130000 * 32, &s);
    CHECK_EQ(s.index, 1);
    CHECK_EQ(speed_permille(&s, 130000 * 32), (130000 - 101000) * 1000 / (157800 - 101000));
    speed_at(&t, 203 * SR - 1, &s);                            // last speed runs to the end
    CHECK_EQ(s.index, 2);
    CHECK_EQ(s.end, 203 * SR);
    CHECK_EQ(speed_start(&t, 1), 101000 * 32);
    CHECK_EQ(speed_start(&t, 9), 157800 * 32);                 // clamps to the last speed

    tune_info_t none = {.sample_rate = SR, .total_samples = 60 * SR, .marker_count = 0};
    speed_at(&none, 30 * SR, &s);                              // no markers: one speed, the whole tune
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.start, 0);
    CHECK_EQ(s.end, 60 * SR);
    CHECK_EQ(speed_permille(&s, 30 * SR), 500);
    CHECK_EQ(speed_start(&none, 2), 0);

    CHECK(strcmp(speed_name(0, 3), "Slow") == 0);
    CHECK(strcmp(speed_name(1, 3), "Medium") == 0);
    CHECK(strcmp(speed_name(2, 3), "Fast") == 0);
    CHECK(strcmp(speed_name(1, 2), "Fast") == 0);
    CHECK(strcmp(speed_name(0, 1), "") == 0);
    CHECK(speed_name(3, 5) == NULL);
}

static void test_resume_find(void)
{
    FILE *f = fopen("tests/host/fixtures/index_v2.bin", "rb");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    uint8_t buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    tune_index_t ix;
    CHECK_EQ(tune_index_open(&ix, buf, n), TUNE_INDEX_OK);
    uint16_t tune = 99;
    uint32_t sample = 0;
    CHECK(resume_find(&ix, 0x1a7c2172u, 1500, &tune, &sample));    // Clare Jig, 3 s long in the fixture
    CHECK_EQ(tune, 0);
    CHECK_EQ(sample, 48000);
    tune_info_t t;
    tune_index_get(&ix, 1, &t);
    CHECK(resume_find(&ix, t.tune_id, 0, &tune, &sample));
    CHECK_EQ(tune, 1);
    CHECK(resume_find(&ix, 0x1a7c2172u, 3000, &tune, &sample));    // saved at the end: back to its first marker
    CHECK_EQ(tune, 0);
    CHECK_EQ(sample, 500 * 32);
    CHECK(!resume_find(&ix, 0xdeadbeefu, 0, &tune, &sample));      // tune removed
}

static void test_taps(void)
{
    tap_detector_t d = {0};
    CHECK_EQ(tap_detector_poll(&d, 0), TAP_NONE);
    CHECK_EQ(tap_detector_tap(&d, 1000), TAP_NONE);
    CHECK_EQ(tap_detector_poll(&d, 1299), TAP_NONE);              // still waiting for a second tap
    CHECK_EQ(tap_detector_poll(&d, 1300), TAP_SINGLE);
    CHECK_EQ(tap_detector_poll(&d, 1400), TAP_NONE);              // reported once
    CHECK_EQ(tap_detector_tap(&d, 2000), TAP_NONE);
    CHECK_EQ(tap_detector_tap(&d, 2250), TAP_DOUBLE);
    CHECK_EQ(tap_detector_poll(&d, 3000), TAP_NONE);              // a double is not also a single
    CHECK_EQ(tap_detector_tap(&d, 4000), TAP_NONE);
    CHECK_EQ(tap_detector_tap(&d, 4400), TAP_NONE);               // too slow: this starts a new tap
    CHECK_EQ(tap_detector_poll(&d, 4500), TAP_NONE);
    CHECK_EQ(tap_detector_poll(&d, 4700), TAP_SINGLE);
}

static void test_resume_saving(void)
{
    resume_saver_t r = {0};
    CHECK(resume_should_save(&r, 0, 7, 1000, true));              // nothing saved yet
    CHECK(!resume_should_save(&r, 1000, 7, 2000, true));
    CHECK(!resume_should_save(&r, 29999, 7, 31000, true));
    CHECK(resume_should_save(&r, 30000, 7, 31000, true));         // every 30 s while playing
    CHECK_EQ(r.position_ms, 31000);
    CHECK(resume_should_save(&r, 31000, 8, 6800, true));          // tune change: at once
    CHECK(!resume_should_save(&r, 32000, 8, 8000, false));        // paused: wait until it settles
    CHECK(!resume_should_save(&r, 33999, 8, 8000, false));
    CHECK(resume_should_save(&r, 34000, 8, 8000, false));
    CHECK(!resume_should_save(&r, 40000, 8, 8000, false));        // nothing new
    CHECK(!resume_should_save(&r, 41000, 8, 4000, false));        // seek while paused
    CHECK(resume_should_save(&r, 43000, 8, 4000, false));

    resume_saver_init(&r, 50000, 9, 12000);                       // from NVS at boot: no rewrite
    CHECK(!resume_should_save(&r, 50000, 9, 12000, false));
    CHECK(!resume_should_save(&r, 60000, 9, 12000, false));
}

int main(void)
{
    test_speeds();
    test_resume_find();
    test_taps();
    test_resume_saving();
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
