// Hardware-independent parsing of CST816 touch reports (plain C): unit-tested on the host, see tests/host/.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bytes read from register 0x00 of the CST816 (I2C 0x15). The layout follows the vendor driver
// (docs/vendor/.../08_LVGL_Test/components/lcd_touch_bsp/lcd_touch_bsp.c):
//   [2] = number of touch points, [3] low nibble = X[11:8], [4] = X[7:0], [5] low nibble = Y[11:8], [6] = Y[7:0]
#define CST816_REPORT_LEN 7

typedef struct {
    bool pressed;
    uint16_t x;
    uint16_t y;
} cst816_point_t;

// Decode one report. Coordinates are clamped to [0, width-1] x [0, height-1].
cst816_point_t cst816_parse(const uint8_t report[CST816_REPORT_LEN], uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif
