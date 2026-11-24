idf_component_register(SRCS "main.c" "audio_fe.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_hosted
                    PRIV_REQUIRES nvs_flash esp_wifi esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_timer)
