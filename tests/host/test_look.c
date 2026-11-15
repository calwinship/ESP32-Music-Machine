// Host tests for logic/look (speed colours, speed scale, tune list curves).
#include <stdint.h>
#include <stdio.h>

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

static void test_scale(void)
{
    int32_t x, w;
    scale_segment(0, 3, 96, 6, &x, &w);
    CHECK_EQ(x, 0);
    CHECK_EQ(w, 28);
    scale_segment(2, 3, 96, 6, &x, &w);
    CHECK_EQ(x, 68);
    CHECK(x + w <= 96);
    scale_segment(1, 2, 96, 6, &x, &w);
    CHECK_EQ(x, 51);
    CHECK_EQ(w, 45);
    scale_segment(0, 0, 96, 6, &x, &w);             // no speeds: whole track, no division by zero
    CHECK_EQ(w, 96);

    CHECK_EQ(scale_dot_x(0, 0, 3, 96, 6), 0);
    CHECK_EQ(scale_dot_x(0, 1000, 3, 96, 6), 28);
    CHECK_EQ(scale_dot_x(1, 500, 3, 96, 6), 34 + 14);
    CHECK_EQ(scale_dot_x(2, 2000, 3, 96, 6), 96);  // permille clamps

    CHECK_EQ(scale_speed_at(-20, 3, 96), 0);       // on the "Slow" label
    CHECK_EQ(scale_speed_at(0, 3, 96), 0);
    CHECK_EQ(scale_speed_at(31, 3, 96), 0);
    CHECK_EQ(scale_speed_at(32, 3, 96), 1);
    CHECK_EQ(scale_speed_at(64, 3, 96), 2);
    CHECK_EQ(scale_speed_at(95, 3, 96), 2);
    CHECK_EQ(scale_speed_at(200, 3, 96), 2);       // on the "Fast" label
    CHECK_EQ(scale_speed_at(47, 2, 96), 0);
    CHECK_EQ(scale_speed_at(48, 2, 96), 1);
    CHECK_EQ(scale_speed_at(50, 1, 96), 0);
    CHECK_EQ(scale_speed_at(50, 0, 96), 0);
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

    const int32_t r = 130, band = 22, half = 620;
    CHECK(scale_hit(0, -130, r, band, half));      // on the arc at the top
    CHECK(scale_hit(0, -150, r, band, half));      // just outside it
    CHECK(scale_hit(0, -110, r, band, half));      // just inside it
    CHECK(!scale_hit(0, -100, r, band, half));     // the tune icon below: not the scale
    CHECK(!scale_hit(0, -160, r, band, half));     // the battery above
    CHECK(scale_hit(100, -83, r, band, half));     // the "Fast" label (50 degrees right)
    CHECK(!scale_hit(130, 0, r, band, half));      // 90 degrees round: not the scale
    CHECK(!scale_hit(0, 130, r, band, half));      // bottom

    // A tap's angle -> speed, with the track from -38 to +38 degrees.
    CHECK_EQ(scale_speed_at(angle_d10(-100, -83) + 380, 3, 760), 0);   // on "Slow"
    CHECK_EQ(scale_speed_at(angle_d10(0, -130) + 380, 3, 760), 1);     // top = the middle speed
    CHECK_EQ(scale_speed_at(angle_d10(100, -83) + 380, 3, 760), 2);    // on "Fast"
}

int main(void)
{
    test_speed_colours();
    test_scale();
    test_angles();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
