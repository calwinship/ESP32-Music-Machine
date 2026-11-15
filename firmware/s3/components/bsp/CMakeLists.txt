# Board drivers. lvgl/lvgl and esp_lcd_sh8601 come from this component's idf_component.yml.
idf_component_register(
    SRCS "bsp_display.c" "bsp_knob.c" "bsp_audio.c" "bsp_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_lcd esp_timer fatfs vfs logic)
