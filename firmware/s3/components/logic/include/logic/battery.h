// Battery level from the supply voltage (BATT_ADC). Plain C, host-tested (tests/host/test_battery.c).
//
// What the pin measures (docs/hardware.md, GPIO1): on battery the "5V" net is the cell itself (4.02-4.06 V measured);
// on USB it is the USB rail (4.66-4.78 V measured), which says nothing about the cell. So above BATTERY_USB_MV we
// report "plugged in" and no level.
// The voltage -> level curve is a generic lithium-cell curve with "empty" where the 3.3 V regulator starts to drop
// out: UNVERIFIED on this board (one reading so far), so the UI shows it only as a coarse icon, never a number.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BATTERY_USB_MV     4400   // between a full cell (4.2 V) and the lowest USB reading (4.66 V)
#define BATTERY_EMPTY_MV   3450
#define BATTERY_FULL_MV    4150
#define BATTERY_SMOOTH     16     // exponential average over ~16 samples (the UI samples every 2 s: ~30 s)
#define BATTERY_DROP_STEP  5      // shown level follows a fall of this many points...
#define BATTERY_RISE_STEP  10     // ...but a rise only this big (a cell recovers a little when the load drops)

typedef struct {
    bool known;            // false until the first good sample
    bool plugged;
    uint8_t level;         // shown level, percent (0-100)
} battery_state_t;

typedef struct {
    battery_state_t state;
    uint32_t avg_x16;      // smoothed mV x BATTERY_SMOOTH
} battery_t;

// Level 0-100 for a cell voltage (piecewise linear; clamps).
uint8_t battery_level_from_mv(uint32_t mv);

void battery_init(battery_t *b);
// Feed one reading (mV; 0 = ADC failed, ignored). Returns the state to show.
battery_state_t battery_update(battery_t *b, uint32_t mv);

#ifdef __cplusplus
}
#endif
