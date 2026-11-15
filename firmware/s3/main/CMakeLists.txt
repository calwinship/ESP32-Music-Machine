# esp_audio_codec comes from main/idf_component.yml (added to this component's requirements automatically).
# Latin-1 UI fonts are optional generated files (tools/gen_fonts.sh, which touches this file so the glob reruns);
# with them titles show fadas (ui_fonts.h).
file(GLOB FONT_SRCS "${CMAKE_CURRENT_LIST_DIR}/fonts/font_*.c")
idf_component_register(
    SRCS "main.c" "player_task.c" "settings.c" "ui.c" ${FONT_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp logic nvs_flash)
if(FONT_SRCS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UI_LATIN1_FONTS=1)
endif()
