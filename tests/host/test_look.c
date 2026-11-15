// Host tests for logic/look (speed colours, speed scale, tune list curves).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LLABS(x) llabs((long long)(x))

#include "logic/look.h"

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

static void test_speed_colours(void)
{
    CHECK_EQ(speed_rgb(0, 1), SPEED_RGB_MEDIUM);
    CHECK_EQ(speed_rgb(0, 0), SPEED_RGB_MEDIUM);
    CHECK_EQ(speed_rgb(0, 2), SPEED_RGB_SLOW);
    CHECK_EQ(speed_rgb(1, 2), SPEED_RGB_FAST);
    CHECK_EQ(speed_rgb(0, 3), SPEED_RGB_SLOW);
    CHECK_EQ(speed_rgb(1, 3), SPEED_RGB_MEDIUM);
    CHECK_EQ(speed_rgb(2, 3), SPEED_RGB_FAST);
    CHECK_EQ(speed_rgb(0, 5), SPEED_RGB_SLOW);
    CHECK_EQ(speed_rgb(2, 5), SPEED_RGB_MEDIUM);   // the middle of an odd count is exactly amber
    CHECK_EQ(speed_rgb(4, 5), SPEED_RGB_FAST);
    CHECK_EQ(speed_rgb(9, 3), SPEED_RGB_FAST);     // out of range clamps
    uint32_t c = speed_rgb(1, 5);                   // half-way teal -> amber
    CHECK_EQ((c >> 16) & 0xFF, (0x2E + 0xF2 + 1) / 2);
    CHECK_EQ(c & 0xFF, (0xB6 + 0x3A + 1) / 2);
    CHECK(speed_rgb(7, 8) != 0 && speed_rgb(7, 8) <= 0xFFFFFF);
}

static void test_timeline(void)
{
    // Clare Jig-like: 3 speeds, 0..101 s, 101..158 s, 158..203.5 s (ms).
    const uint32_t b[] = {0, 101000, 157800, 203520};
    int32_t a0[3], a1[3];
    timeline_layout(b, 3, 40, a0, a1);
    CHECK_EQ(a0[0], 20);                            // half a gap after the top
    CHECK_EQ(a1[2], 3600 - 20);                     // half a gap before the top
    CHECK_EQ(a0[1] - a1[0], 40);                    // gaps between segments
    CHECK_EQ(a0[2] - a1[1], 40);
    int32_t room = 3600 - 3 * 40;                   // lengths in proportion to the durations
    CHECK(LLABS((a1[0] - a0[0]) - (int64_t)room * 101000 / 203520) <= 1);
    CHECK(LLABS((a1[2] - a0[2]) - (int64_t)room * 45720 / 203520) <= 1);

    CHECK_EQ(timeline_played_d10(b, 0, 0, a0, a1), a0[0]);
    CHECK_EQ(timeline_played_d10(b, 0, 101000, a0, a1), a1[0]);
    CHECK_EQ(timeline_played_d10(b, 1, 50000, a0, a1), a0[1]);   // before the segment: nothing played
    int32_t mid = timeline_played_d10(b, 1, (101000 + 157800) / 2, a0, a1);
    CHECK(LLABS(mid - (a0[1] + a1[1]) / 2) <= 1);
    CHECK_EQ(timeline_played_d10(b, 2, 999999, a0, a1), a1[2]);

    CHECK_EQ(timeline_segment_at(a0[1] + 5, 3, a0, a1), 1);
    CHECK_EQ(timeline_segment_at(a1[0] + 10, 3, a0, a1), 0);    // in the gap, nearer segment 0
    CHECK_EQ(timeline_segment_at(a0[1] - 10, 3, a0, a1), 1);    // in the gap, nearer segment 1
    CHECK_EQ(timeline_segment_at(5, 3, a0, a1), 0);             // the top gap, just right of the top
    CHECK_EQ(timeline_segment_at(-5, 3, a0, a1), 2);            // just left of the top (negative angle)
    CHECK_EQ(timeline_segment_at(3595, 3, a0, a1), 2);
    CHECK_EQ(timeline_segment_at(1800, 1, a0, a1), 0);

    const uint32_t one[] = {0, 53000};               // single speed: one segment, nearly the whole circle
    timeline_layout(one, 1, 40, a0, a1);
    CHECK_EQ(a0[0], 20);
    CHECK_EQ(a1[0], 3580);
    const uint32_t flat[] = {5, 5, 5};               // zero-length tune: equal shares, no division by zero
    timeline_layout(flat, 2, 40, a0, a1);
    CHECK_EQ(a1[0] - a0[0], a1[1] - a0[1]);
}

static void test_angles(void)
{
    CHECK_EQ(angle_d10(0, -100), 0);               // straight up
    CHECK_EQ(angle_d10(100, 0), 900);              // right
    CHECK_EQ(angle_d10(-100, 0), -900);            // left
    CHECK_EQ(angle_d10(100, -100), 450);
    CHECK_EQ(angle_d10(-100, -100), -450);
    CHECK_EQ(angle_d10(0, 0), 0);
    CHECK(angle_d10(0, 100) == 1800 || angle_d10(0, 100) == -1800);   // straight down

    CHECK(ring_hit(0, -173, 148, 190));            // on the ring
    CHECK(ring_hit(150, 0, 148, 190));
    CHECK(!ring_hit(0, -126, 148, 190));           // the tune icon / list button: not the ring
    CHECK(!ring_hit(0, 0, 148, 190));
    CHECK(!ring_hit(150, 150, 148, 190));          // off the round screen
}

int main(void)
{
    test_speed_colours();
    test_timeline();
    test_angles();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
