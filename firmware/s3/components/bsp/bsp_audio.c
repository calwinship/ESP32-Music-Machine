// Audio out: I2S1 standard TX to the PCM5100A (via the CH445P switch). Pins and the GPIO0 switch level come from the
// vendor's verified 07_Audio_Test. Deliberate differences from the vendor (docs/hardware.md, "Known quirks"):
//  - standard Philips I2S (I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG): the DAC's FMT pin is tied low = I2S. The vendor
//    uses left-justified (MSB) format.
//  - gain set by the app (bsp_audio_set_gain_q15, volume steps in logic/controls.h). Until it is set it is 0.1
//    (-20 dB), the same as the vendor demo's volume 10 used in the first listening tests.
//  - TX underruns are counted (on_send_q_ovf) and the counter is self-tested at init.
#include <stdatomic.h>
#include <string.h>

#include "bsp/bsp.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_audio";

#define AUDIO_I2S_PORT     I2S_NUM_1   // as the vendor; I2S0 is the only one with PDM RX (microphone)
#define AUDIO_DMA_DESC     8
#define AUDIO_DMA_FRAMES   480         // per descriptor: 15 ms at 32 kHz; 8 x 15 = 120 ms of buffering
#define AUDIO_CHUNK        576         // mono samples converted per i2s_channel_write
#define AUDIO_WRITE_MS     1000
#define SELFTEST_MS        300         // > 8 buffers' worth, so the queue fills and underruns must fire
#define AUDIO_GAIN_START   3277        // 0.1 in Q15

static i2s_chan_handle_t s_tx;
static atomic_uint s_underruns;
static atomic_uint s_gain = AUDIO_GAIN_START;
static uint32_t s_selftest;
static int16_t s_stereo[AUDIO_CHUNK * 2];

// ISR: the DMA finished a buffer while every buffer was already waiting to be refilled, i.e. it is now sending a
// buffer the writer never refilled (auto_clear makes that silence).
static bool IRAM_ATTR on_underrun(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
    (void)h;
    (void)e;
    (void)ctx;
    atomic_fetch_add_explicit(&s_underruns, 1, memory_order_relaxed);
    return false;
}

esp_err_t bsp_audio_init(uint32_t sample_rate)
{
    const gpio_config_t sw = {
        .pin_bit_mask = 1ULL << BSP_DAC_SWITCH_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   // as the vendor (R58 also pulls it up)
    };
    ESP_RETURN_ON_ERROR(gpio_config(&sw), TAG, "dac switch gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_DAC_SWITCH_PIN, 1), TAG, "dac switch level");

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan.dma_desc_num = AUDIO_DMA_DESC;
    chan.dma_frame_num = AUDIO_DMA_FRAMES;
    chan.auto_clear = true;   // an underrun sends silence, not a stale buffer
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "new channel");

    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // PCM5100A SCK is tied to GND: its PLL runs from BCK
            .bclk = BSP_I2S_PIN_BCK,
            .ws = BSP_I2S_PIN_WS,
            .dout = BSP_I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "std mode");
    const i2s_event_callbacks_t cbs = {.on_send_q_ovf = on_underrun};
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_tx, &cbs, NULL), TAG, "callbacks");

    // Self-test: run with no data. The counter must move, otherwise a later "0 underruns" proves nothing.
    atomic_store(&s_underruns, 0);
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable (self-test)");
    vTaskDelay(pdMS_TO_TICKS(SELFTEST_MS));
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "disable (self-test)");
    s_selftest = atomic_exchange(&s_underruns, 0);
    if (s_selftest) {
        ESP_LOGI(TAG, "underrun self-test: %u underruns in %d ms with no data (counter works)", (unsigned)s_selftest,
                 SELFTEST_MS);
    } else {
        ESP_LOGE(TAG, "underrun self-test: 0 underruns in %d ms with no data: the counter is NOT trustworthy",
                 SELFTEST_MS);
    }
    ESP_LOGI(TAG, "I2S%d std Philips 16-bit stereo, %u Hz, DMA %d x %d frames (%u ms)", AUDIO_I2S_PORT,
             (unsigned)sample_rate, AUDIO_DMA_DESC, AUDIO_DMA_FRAMES,
             (unsigned)(AUDIO_DMA_DESC * AUDIO_DMA_FRAMES * 1000u / sample_rate));
    return ESP_OK;
}

esp_err_t bsp_audio_start(void)
{
    // Preload silence into every DMA buffer so the first buffers sent are ours, then start the clocks.
    memset(s_stereo, 0, sizeof(s_stereo));
    size_t loaded;
    do {
        ESP_RETURN_ON_ERROR(i2s_channel_preload_data(s_tx, s_stereo, sizeof(s_stereo), &loaded), TAG, "preload");
    } while (loaded == sizeof(s_stereo));
    return i2s_channel_enable(s_tx);
}

esp_err_t bsp_audio_stop(void)
{
    return i2s_channel_disable(s_tx);
}

esp_err_t bsp_audio_write_mono(const int16_t *pcm, size_t samples)
{
    const int32_t gain = (int32_t)atomic_load_explicit(&s_gain, memory_order_relaxed);
    while (samples) {
        size_t n = samples < AUDIO_CHUNK ? samples : AUDIO_CHUNK;
        for (size_t i = 0; i < n; i++) {
            int16_t v = pcm ? (int16_t)(((int32_t)pcm[i] * gain) >> 15) : 0;
            s_stereo[2 * i] = v;
            s_stereo[2 * i + 1] = v;
        }
        size_t written;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, s_stereo, n * 4, &written, AUDIO_WRITE_MS), TAG, "write");
        if (pcm) {
            pcm += n;
        }
        samples -= n;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_write_silence(size_t samples)
{
    return bsp_audio_write_mono(NULL, samples);
}

void bsp_audio_set_gain_q15(uint16_t gain)
{
    atomic_store_explicit(&s_gain, gain > 32767 ? 32767u : gain, memory_order_relaxed);
}

uint32_t bsp_audio_underruns(void)
{
    return atomic_load_explicit(&s_underruns, memory_order_relaxed);
}

uint32_t bsp_audio_selftest_underruns(void)
{
    return s_selftest;
}
