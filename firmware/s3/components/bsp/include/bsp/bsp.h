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

// Backlight PWM duty 0 (off) .. 255 (full). bsp_display_start leaves it off.
void bsp_backlight_set(uint8_t duty);
// Ramp the backlight from off to full over about `ms` (non-blocking).
void bsp_backlight_fade_in(uint32_t ms);

// Supply voltage on BATT_ADC (the "5V" net), in mV, averaged over a few reads; 0 if the ADC failed.
// bsp_battery_init is called by the first read.
uint32_t bsp_supply_mv(void);

// Start sampling knob #1 every BSP_KNOB_SAMPLE_MS ms.
esp_err_t bsp_knob_start(void);
// Steps since the last call (+ clockwise, - anticlockwise); clears the count. Safe from any task.
int32_t bsp_knob_take_delta(void);

// ---- audio: I2S -> CH445P -> PCM5100A -> 3.5 mm jack. Standard (Philips) I2S, 16-bit stereo, no MCLK. ----------
// Selects the S3 on the DAC switch, configures the channel (left disabled) and self-tests the underrun counter:
// it runs the channel with no data for 300 ms and logs how many underruns that produced (must be > 0).
esp_err_t bsp_audio_init(uint32_t sample_rate);
// Fill the DMA buffers with silence and start the clocks. Write audio right after.
esp_err_t bsp_audio_start(void);
// Stop the clocks (no underruns are counted while stopped).
esp_err_t bsp_audio_stop(void);
// Blocking write of mono samples, sent identically on left and right.
esp_err_t bsp_audio_write_mono(const int16_t *pcm, size_t samples);
esp_err_t bsp_audio_write_silence(size_t samples);
// Output gain, Q15 (32767 = 1.0), applied to every following write. Safe from any task.
void bsp_audio_set_gain_q15(uint16_t gain);
// Times the DMA sent a buffer that had not been refilled (a TX underrun) since bsp_audio_init's self-test.
uint32_t bsp_audio_underruns(void);
// Underruns the self-test produced (0 would mean the counter cannot be trusted).
uint32_t bsp_audio_selftest_underruns(void);

// ---- storage: the `tunes` partition, read-only FAT ----------------------------------------------------------
// Tunes are opened by path under this mount point only, so an SD card mounted here can replace the partition.
#define BSP_TUNES_PATH "/tunes"
esp_err_t bsp_storage_mount(void);

#ifdef __cplusplus
}
#endif
