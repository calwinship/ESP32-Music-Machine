// Board support: the only code that touches ESP32-S3 peripherals. Logic lives in components/logic (host-tested).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "bsp/board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

// Backlight, QSPI panel, I2C + touch, LVGL (v8) port with its own task. Call once.
esp_err_t bsp_display_start(void);

// LVGL is not thread-safe: wrap every LVGL call made outside LVGL callbacks/timers in lock/unlock.
// timeout_ms < 0 waits forever. LVGL timers and event callbacks already run with the lock held.
bool bsp_display_lock(int timeout_ms);
void bsp_display_unlock(void);

// Backlight PWM duty 0 (off) .. 255 (full).
void bsp_backlight_set(uint8_t duty);

// Start sampling knob #1 every BSP_KNOB_SAMPLE_MS ms.
esp_err_t bsp_knob_start(void);
// Steps since the last call (+ clockwise, - anticlockwise); clears the count. Safe from any task.
int32_t bsp_knob_take_delta(void);

#ifdef __cplusplus
}
#endif
