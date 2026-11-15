#include "settings.h"

#include "esp_log.h"
#include "logic/controls.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NS          "player"
#define KEY_VOLUME  "volume"
#define KEY_TUNE    "tune_id"
#define KEY_POS     "pos_ms"
#define KEY_GUIDE   "guide_seen"

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition holds data in a layout this IDF can't use (e.g. from the factory firmware): start fresh.
        ESP_LOGW(TAG, "NVS: %s, erasing it", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

int32_t settings_volume(void)
{
    nvs_handle_t h;
    int32_t v = VOLUME_DEFAULT;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, KEY_VOLUME, &v) != ESP_OK || !volume_valid(v)) {
            v = VOLUME_DEFAULT;
        }
        nvs_close(h);
    }
    return v;
}

bool settings_resume(uint32_t *tune_id, uint32_t *position_ms)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_u32(h, KEY_TUNE, tune_id) == ESP_OK && nvs_get_u32(h, KEY_POS, position_ms) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

static void save(const char *what, esp_err_t (*write)(nvs_handle_t, uint32_t, uint32_t), uint32_t a, uint32_t b)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = write(h, a, b);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving %s failed: %s", what, esp_err_to_name(err));
    }
}

static esp_err_t write_volume(nvs_handle_t h, uint32_t v, uint32_t unused)
{
    (void)unused;
    return nvs_set_i32(h, KEY_VOLUME, (int32_t)v);
}

static esp_err_t write_resume(nvs_handle_t h, uint32_t id, uint32_t ms)
{
    esp_err_t err = nvs_set_u32(h, KEY_TUNE, id);
    return err == ESP_OK ? nvs_set_u32(h, KEY_POS, ms) : err;
}

void settings_save_volume(int32_t volume)
{
    save("volume", write_volume, (uint32_t)volume, 0);
    ESP_LOGI(TAG, "volume %d saved", (int)volume);
}

void settings_save_resume(uint32_t tune_id, uint32_t position_ms)
{
    save("resume", write_resume, tune_id, position_ms);
    ESP_LOGI(TAG, "resume point saved: tune %08x at %u ms", (unsigned)tune_id, (unsigned)position_ms);
}

bool settings_guide_seen(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, KEY_GUIDE, &v) != ESP_OK) {
            v = 0;
        }
        nvs_close(h);
    }
    return v != 0;
}

static esp_err_t write_guide(nvs_handle_t h, uint32_t unused_a, uint32_t unused_b)
{
    (void)unused_a;
    (void)unused_b;
    return nvs_set_u8(h, KEY_GUIDE, 1);
}

void settings_save_guide_seen(void)
{
    save("guide", write_guide, 0, 0);
    ESP_LOGI(TAG, "guide marked as seen");
}
