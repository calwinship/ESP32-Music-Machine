// Supply sense (BATT_ADC, GPIO1 = ADC1 channel 0), as in the vendor's 01_ADC_Test/components/adc_bsp/adc_bsp.c:
// 12 dB attenuation, 12-bit, curve-fitting calibration, x2 for the 10k/10k divider.
#include "bsp/board_pins.h"
#include "bsp/bsp.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "bsp_battery";

#define READS 8

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_failed;

static bool battery_init(void)
{
    if (s_adc || s_failed) {
        return s_adc != NULL;
    }
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BSP_BATT_ADC_UNIT,
        .chan = BSP_BATT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = BSP_BATT_ADC_UNIT};
    const adc_oneshot_chan_cfg_t chan_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK ||
        adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK ||
        adc_oneshot_config_channel(s_adc, BSP_BATT_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed");
        s_failed = true;
        s_adc = NULL;
        return false;
    }
    return true;
}

uint32_t bsp_supply_mv(void)
{
    if (!battery_init()) {
        return 0;
    }
    int sum = 0, n = 0;
    for (int i = 0; i < READS; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BSP_BATT_ADC_CHANNEL, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    return n ? (uint32_t)(sum / n) * BSP_BATT_DIVIDER : 0;
}
