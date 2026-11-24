file(GLOB EMOTE_FILES emote/*.eaf)

idf_component_register(
    SRCS phone_app_assistant.cpp assistant_chat.cpp assistant_emote.cpp assistant_mcp.cpp assistant_wake.cpp assistant_afe.cpp phone_assistant_icon.c eaf/gfx_eaf_dec.c
    INCLUDE_DIRS .
    PRIV_INCLUDE_DIRS eaf
    REQUIRES brookesia_core esp32p4c5_bsp phone_app_settings esp_xiaozhi esp_event espressif__esp-sr
    EMBED_FILES ${EMOTE_FILES}
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
