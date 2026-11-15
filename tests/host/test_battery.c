// Host tests for logic/battery (supply voltage -> battery icon state).
#include <stdint.h>
#include <stdio.h>

#include "logic/battery.h"

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

static void test_curve(void)
{
    CHECK_EQ(battery_level_from_mv(0), 0);
    CHECK_EQ(battery_level_from_mv(BATTERY_EMPTY_MV), 0);
    CHECK_EQ(battery_level_from_mv(3300), 0);
    CHECK_EQ(battery_level_from_mv(BATTERY_FULL_MV), 100);
    CHECK_EQ(battery_level_from_mv(4200), 100);
    CHECK_EQ(battery_level_from_mv(3850), 52);           // a curve point exactly
    CHECK_EQ(battery_level_from_mv(3900), 61);           // half-way between 3850 (52) and 3950 (70)
    CHECK(battery_level_from_mv(4040) >= 80 && battery_level_from_mv(4040) <= 85);   // the user's reading
    uint8_t prev = 0;
    for (uint32_t mv = 3000; mv <= 4300; mv += 5) {     // never goes down as the voltage goes up
        uint8_t l = battery_level_from_mv(mv);
        CHECK(l >= prev && l <= 100);
        prev = l;
    }
}

static void test_plugged(void)
{
    battery_t b;
    battery_init(&b);
    CHECK(!battery_update(&b, 0).known);                // ADC failure before any reading: still unknown
    battery_state_t s = battery_update(&b, 4720);       // USB rail
    CHECK(s.known && s.plugged);
    CHECK(battery_update(&b, 4658).plugged);            // lowest USB reading seen
    s = battery_update(&b, 4040);                       // unplugged: level straight from this reading,
    CHECK(!s.plugged);                                  // not dragged down from the 4.7 V average
    CHECK_EQ(s.level, battery_level_from_mv(4040));
    CHECK(battery_update(&b, 4720).plugged);            // plugged in again
    CHECK(!battery_update(&b, 4200).plugged);           // a full cell is not "plugged"
}

static void test_smoothing_and_hysteresis(void)
{
    battery_t b;
    battery_init(&b);
    battery_state_t s = battery_update(&b, 4040);
    uint8_t start = s.level;
    s = battery_update(&b, 3700);                       // one low reading (e.g. a load spike): small effect
    CHECK(s.level == start);
    for (int i = 0; i < 4; i++) {
        s = battery_update(&b, 4040);
    }
    CHECK_EQ(s.level, start);
    s = battery_update(&b, 0);                          // ADC failure: ignored
    CHECK_EQ(s.level, start);

    for (int i = 0; i < 60; i++) {                       // a real fall is followed
        s = battery_update(&b, 3850);
    }
    CHECK(s.level <= 55 && s.level >= 50);
    uint8_t low = s.level;
    for (int i = 0; i < 60; i++) {                       // a small recovery is not shown...
        s = battery_update(&b, 3880);
    }
    CHECK_EQ(s.level, low);
    for (int i = 0; i < 80; i++) {                       // ...a big rise is (e.g. a different, charged cell)
        s = battery_update(&b, 4100);
    }
    CHECK(s.level >= 85);
    for (int i = 0; i < 200; i++) {                      // empty is always shown
        s = battery_update(&b, 3400);
    }
    CHECK_EQ(s.level, 0);
}

int main(void)
{
    test_curve();
    test_plugged();
    test_smoothing_and_hysteresis();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
