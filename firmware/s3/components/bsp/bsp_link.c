// USB link to ./sync-tunes over the USB-Serial-JTAG port, shared with the log console.
// Installing the driver moves the console onto it too (usb_serial_jtag_vfs_use_driver), so log text and link frames
// go through one queue and a frame is never split by a log line (each frame is one write). With nothing reading the
// port (e.g. on battery) the console drops text after 50 ms instead of blocking (IDF usb_serial_jtag_vfs.c).
#include "bsp/bsp.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "bsp_link";

esp_err_t bsp_link_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 4096, .rx_buffer_size = 8192};
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG, "driver install");
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
}

int bsp_link_read(void *buf, size_t len, int timeout_ms)
{
    return usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(timeout_ms));
}

bool bsp_link_write(const void *buf, size_t len)
{
    // One ring-buffer item, so the frame stays in one piece (callers keep frames far below the 4 KB buffer).
    return usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(200)) == (int)len;
}

void bsp_link_flush(int timeout_ms)
{
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(timeout_ms));
}
