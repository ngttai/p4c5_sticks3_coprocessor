idf_component_register(
    SRCS phone_spectrum.cpp
    INCLUDE_DIRS .
    REQUIRES lvgl__lvgl esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
