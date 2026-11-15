# esp_audio_codec comes from main/idf_component.yml (added to this component's requirements automatically).
idf_component_register(
    SRCS "main.c" "player_task.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp logic nvs_flash)
