idf_component_register(
    SRCS phone_app_radio.cpp radio_player.cpp radio_stations.cpp phone_radio_icon.c
    INCLUDE_DIRS .
    REQUIRES brookesia_core esp32p4c5_bsp phone_app_settings phone_spectrum espressif__cjson mbedtls
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
