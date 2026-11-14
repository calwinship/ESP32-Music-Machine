// Knob #1 sampling: GPIO reads every BSP_KNOB_SAMPLE_MS on an esp_timer; the decoding itself is logic/knob.h.
#include <stdatomic.h>

#include "bsp/bsp.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "logic/knob.h"

static const char *TAG = "bsp_knob";

static knob_decoder_t s_decoder;   // only touched from the timer callback
static atomic_int_fast32_t s_delta;

static void knob_sample_cb(void *arg)
{
    (void)arg;
    int step = knob_decoder_step(&s_decoder, gpio_get_level(BSP_KNOB_PIN_A), gpio_get_level(BSP_KNOB_PIN_B));
    if (step) {
        atomic_fetch_add(&s_delta, step);
    }
}

esp_err_t bsp_knob_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_KNOB_PIN_A) | (1ULL << BSP_KNOB_PIN_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // as the vendor does, in addition to the external 10 kΩ
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio");
    knob_decoder_init(&s_decoder);
    const esp_timer_create_args_t args = {.callback = knob_sample_cb, .name = "knob"};
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, BSP_KNOB_SAMPLE_MS * 1000), TAG, "timer start");
    ESP_LOGI(TAG, "knob sampling every %d ms (A=GPIO%d, B=GPIO%d)", BSP_KNOB_SAMPLE_MS, BSP_KNOB_PIN_A,
             BSP_KNOB_PIN_B);
    return ESP_OK;
}

int32_t bsp_knob_take_delta(void)
{
    return (int32_t)atomic_exchange(&s_delta, 0);
}
