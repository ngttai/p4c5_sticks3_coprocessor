idf_component_register(SRCS "iperf_example_main.c" "link_test.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event console esp_driver_i2s esp_driver_gpio esp_timer
                                  cmd_system esp-qa__wifi-cmd espressif__iperf espressif__iperf-cmd esp-qa__ping-cmd)
