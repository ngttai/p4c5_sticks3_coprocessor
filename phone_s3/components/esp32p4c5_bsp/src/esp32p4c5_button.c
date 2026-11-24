/*
 * BOOT (GPIO35) and IO0 (GPIO0) buttons on the KSDIY ESP32-P4C5 board. Active low, external pull-ups.
 * Exposed through espressif/button, as in esp-bsp.
 */
#include "button_gpio.h"
#include "esp_check.h"
#include "bsp/esp32p4c5_bsp.h"

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size)
{
    if (btn_array == NULL || btn_array_size < BSP_BUTTON_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    if (btn_cnt) {
        *btn_cnt = 0;
    }
    const button_config_t btn_cfg = {0};
    static const gpio_num_t pins[BSP_BUTTON_NUM] = {
        [BSP_BUTTON_BOOT] = BSP_BUTTON_BOOT_IO,
        [BSP_BUTTON_IO0] = BSP_BUTTON_IO0_IO,
    };
    for (int i = 0; i < BSP_BUTTON_NUM; i++) {
        const button_gpio_config_t gpio_cfg = {
            .gpio_num = pins[i],
            .active_level = 0,
        };
        ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn_array[i]),
                            "bsp_button", "button %d create failed", i);
        if (btn_cnt) {
            (*btn_cnt)++;
        }
    }
    return ESP_OK;
}
