// ESP32-S3 pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
// Every value here is cited in docs/hardware.md ("ESP32-S3 GPIO map"): schematic p2 net table, cross-checked with the
// vendor demos. Do not add a pin that is not in that table.
#pragma once
#include "driver/gpio.h"

// Display: QSPI on SPI2 (LCD_QSPI_*), 360x360
#define BSP_LCD_SPI_HOST     SPI2_HOST
#define BSP_LCD_PIN_PCLK     GPIO_NUM_13
#define BSP_LCD_PIN_CS       GPIO_NUM_14
#define BSP_LCD_PIN_D0       GPIO_NUM_15
#define BSP_LCD_PIN_D1       GPIO_NUM_16
#define BSP_LCD_PIN_D2       GPIO_NUM_17
#define BSP_LCD_PIN_D3       GPIO_NUM_18
#define BSP_LCD_PIN_RST      GPIO_NUM_21
#define BSP_LCD_PIN_BL       GPIO_NUM_47   // LCD_BLK: active-high (AO3400A low-side switch)
#define BSP_LCD_H_RES        360
#define BSP_LCD_V_RES        360

// I2C0: shared by touch (CST816 @0x15) and haptics (DRV2605L @0x5A); 5.1 kΩ external pull-ups
#define BSP_I2C_PORT         I2C_NUM_0
#define BSP_I2C_PIN_SDA      GPIO_NUM_11
#define BSP_I2C_PIN_SCL      GPIO_NUM_12
#define BSP_I2C_HZ           300000        // as in the vendor demo
#define BSP_TOUCH_ADDR       0x15
#define BSP_TOUCH_PIN_INT    GPIO_NUM_9    // not used yet (vendor polls)
#define BSP_TOUCH_PIN_RST    GPIO_NUM_10   // not driven (as in the vendor demo)

// Knob #1 (SW2): A pulses on clockwise, B on anticlockwise (bidirectional switch, not quadrature)
#define BSP_KNOB_PIN_A       GPIO_NUM_8    // EC1_A
#define BSP_KNOB_PIN_B       GPIO_NUM_7    // EC1_B
#define BSP_KNOB_SAMPLE_MS   3             // as in the vendor's bidi_switch_knob.c (TICKS_INTERVAL)
