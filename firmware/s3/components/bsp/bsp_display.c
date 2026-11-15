// Display + touch + LVGL port. Restructured from the vendor's verified 08_LVGL_Test/main/main.c and its BSP
// components; the hardware sequence (backlight -> SPI bus -> panel IO -> SH8601 panel -> reset/init -> I2C/touch ->
// LVGL) and all parameters are unchanged unless a comment says otherwise.
#include "bsp/bsp.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "lcd_init_cmds.h"
#include "logic/cst816.h"

static const char *TAG = "bsp_display";

#define LVGL_TICK_MS        2
#define LVGL_TASK_MAX_MS    500
#define LVGL_TASK_MIN_MS    5
#define LVGL_TASK_STACK     (4 * 1024)
#define LVGL_TASK_PRIO      2
#define LVGL_BUF_LINES      (BSP_LCD_V_RES / 10)

static SemaphoreHandle_t s_lvgl_mux;
static i2c_master_dev_handle_t s_touch;

// ---- backlight (vendor lcd_bl_pwm_bsp.c: LEDC timer 3, channel 1, 50 kHz, 8-bit) --------------------------------
static esp_err_t backlight_init(uint8_t duty)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 50 * 1000,
        .clk_cfg = LEDC_SLOW_CLK_RC_FAST,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    const ledc_channel_config_t ch = {
        .gpio_num = BSP_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_3,
        .duty = duty,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

void bsp_backlight_set(uint8_t duty)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1));
}

// Fade-in: FADE_STEPS duty steps on an esp_timer (runs in the esp_timer task, not an ISR). Duty follows a square
// curve so the brightness looks even to the eye rather than jumping up at the start.
#define FADE_STEPS 12
static esp_timer_handle_t s_fade_timer;
static int s_fade_step;

static void fade_cb(void *arg)
{
    (void)arg;
    s_fade_step++;
    uint32_t duty = 255u * (uint32_t)(s_fade_step * s_fade_step) / (FADE_STEPS * FADE_STEPS);
    bsp_backlight_set((uint8_t)duty);
    if (s_fade_step >= FADE_STEPS) {
        esp_timer_stop(s_fade_timer);
    }
}

void bsp_backlight_fade_in(uint32_t ms)
{
    if (!s_fade_timer) {
        const esp_timer_create_args_t args = {.callback = fade_cb, .name = "bl_fade"};
        if (esp_timer_create(&args, &s_fade_timer) != ESP_OK) {
            bsp_backlight_set(255);
            return;
        }
    }
    esp_timer_stop(s_fade_timer);   // harmless if not running
    s_fade_step = 0;
    uint64_t period_us = (uint64_t)ms * 1000u / FADE_STEPS;
    if (esp_timer_start_periodic(s_fade_timer, period_us ? period_us : 1000u) != ESP_OK) {
        bsp_backlight_set(255);
    }
}

// ---- touch (vendor i2c_bsp.c + lcd_touch_bsp.c) -------------------------------------------------------------------
static esp_err_t touch_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BSP_I2C_PORT,
        .scl_io_num = BSP_I2C_PIN_SCL,
        .sda_io_num = BSP_I2C_PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_TOUCH_ADDR,
        .scl_speed_hz = BSP_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_touch), TAG, "touch dev");
    const uint8_t normal_mode[2] = {0x00, 0x00};  // vendor: write 0x00 to reg 0x00 ("switch to normal mode")
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_touch, normal_mode, sizeof normal_mode, 1000), TAG, "touch init");
    return ESP_OK;
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    uint8_t reg = 0x00;
    uint8_t report[CST816_REPORT_LEN] = {0};
    // Change from vendor: the read result is checked; on error report "released" instead of stale data.
    if (i2c_master_transmit_receive(s_touch, &reg, 1, report, sizeof report, 50) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    cst816_point_t p = cst816_parse(report, BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (p.pressed) {
        data->point.x = (lv_coord_t)p.x;
        data->point.y = (lv_coord_t)p.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---- LVGL glue (vendor main.c) ------------------------------------------------------------------------------------
static bool flush_ready_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

// The panel needs even-aligned windows (vendor example_lvgl_rounder_cb).
static void lvgl_rounder_cb(lv_disp_drv_t *drv, lv_area_t *area)
{
    (void)drv;
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

bool bsp_display_lock(int timeout_ms)
{
    const TickType_t ticks = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, ticks) == pdTRUE;
}

void bsp_display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mux);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started");
    uint32_t delay_ms = LVGL_TASK_MAX_MS;
    for (;;) {
        if (bsp_display_lock(-1)) {
            delay_ms = lv_timer_handler();
            bsp_display_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_MS) {
            delay_ms = LVGL_TASK_MAX_MS;
        } else if (delay_ms < LVGL_TASK_MIN_MS) {
            delay_ms = LVGL_TASK_MIN_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t bsp_display_start(void)
{
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    static lv_indev_drv_t indev_drv;

    // Change from vendor: start with the backlight off (vendor: 255). Until the app has drawn its first frame the
    // panel shows leftover memory and then LVGL's white default screen; the app calls bsp_backlight_fade_in().
    ESP_RETURN_ON_ERROR(backlight_init(0), TAG, "backlight");

    const spi_bus_config_t buscfg = {
        .data0_io_num = BSP_LCD_PIN_D0,
        .data1_io_num = BSP_LCD_PIN_D1,
        .sclk_io_num = BSP_LCD_PIN_PCLK,
        .data2_io_num = BSP_LCD_PIN_D2,
        .data3_io_num = BSP_LCD_PIN_D3,
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_PIN_CS, flush_ready_cb, &disp_drv);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &io), TAG,
                        "panel io");

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = bsp_lcd_init_cmds,
        .init_cmds_size = sizeof(bsp_lcd_init_cmds) / sizeof(bsp_lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,  // CONFIG_LV_COLOR_DEPTH=16 (sdkconfig.defaults)
        .vendor_config = &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(io, &panel_cfg, &panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");

    lv_init();
    const size_t buf_px = BSP_LCD_H_RES * LVGL_BUF_LINES;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buffers");
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = BSP_LCD_H_RES;
    disp_drv.ver_res = BSP_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = panel;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {.callback = lvgl_tick_cb, .name = "lvgl_tick"};
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG, "tick start");

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);   // change from vendor: no white first frame

    // Change from vendor: recursive mutex, so code holding the lock (e.g. inside an LVGL timer) can call helpers
    // that also lock.
    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mux, ESP_ERR_NO_MEM, TAG, "mutex");
    ESP_RETURN_ON_FALSE(xTaskCreate(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "lvgl task");
    ESP_LOGI(TAG, "display started (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}
