#include "logic/cst816.h"

cst816_point_t cst816_parse(const uint8_t report[CST816_REPORT_LEN], uint16_t width, uint16_t height)
{
    cst816_point_t p = {0};
    if (report[2] == 0) {
        return p;
    }
    uint16_t x = (uint16_t)(((report[3] & 0x0F) << 8) | report[4]);
    uint16_t y = (uint16_t)(((report[5] & 0x0F) << 8) | report[6]);
    // The vendor clamps to width/height (one past the last pixel); clamp to the last valid pixel instead.
    p.pressed = true;
    p.x = (width && x >= width) ? (uint16_t)(width - 1) : x;
    p.y = (height && y >= height) ? (uint16_t)(height - 1) : y;
    return p;
}
