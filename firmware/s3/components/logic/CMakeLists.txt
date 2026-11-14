# Pure C logic: no ESP-IDF dependencies, so it also builds and runs on the host (tests/host/).
idf_component_register(
    SRCS "knob.c" "cst816.c"
    INCLUDE_DIRS "include")
