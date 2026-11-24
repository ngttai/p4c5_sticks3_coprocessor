# Kind of splash: Kconfig PHONE_BOOT_SPLASH. splash.bin (logo) is built by tools/make_splash.py from
# tools/boot_logo.json and embedded only for the logo splash.
set(srcs)
set(embed)
if(CONFIG_PHONE_BOOT_SPLASH_LOGO)
    list(APPEND srcs splash_logo.cpp)
    list(APPEND embed splash.bin)
elseif(CONFIG_PHONE_BOOT_SPLASH_SIMPLE)
    list(APPEND srcs splash_simple.cpp splash_font_56.c)
else()
    list(APPEND srcs splash_none.cpp)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS .
    REQUIRES brookesia_core phone_app_settings
    EMBED_FILES ${embed}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE)
