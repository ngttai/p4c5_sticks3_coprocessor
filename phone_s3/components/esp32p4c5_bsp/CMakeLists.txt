idf_component_register(
    SRCS "src/esp32p4c5_bsp.c" "src/esp32p4c5_sdcard.c" "src/esp32p4c5_button.c" "src/esp32p4c5_imu.c" "src/esp32p4c5_audio.c" "src/esp32p4c5_camera.c" "src/esp32p4c5_spiffs.c" "src/esp32p4c5_wifi.c" "src/esp32p4c5_pmic.c" "src/esp32p4c5_led.c" "src/esp32p4c5_cache_fix.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_uart esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_sdmmc fatfs spiffs esp_lcd espressif__button espressif__esp_codec_dev espressif__esp_audio_effects espressif__esp_video espressif__esp_hosted esp_wifi esp_netif esp_event nvs_flash
    PRIV_REQUIRES espressif__led_strip esp_driver_ledc esp_timer esp_system esp_hw_support hal
)

# Keep the ESP32-P4 L1 D-cache arbitration constructor (esp-idf#19369) even when
# the application references nothing in src/esp32p4c5_cache_fix.c.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp32p4c5_bsp_l1_dcache_round_robin")
