/*
 * RGB LED (one WS2812B on GPIO34) for the KSDIY ESP32-P4C5 board, driven through
 * espressif/led_strip (RMT).
 */
#include "esp_check.h"
#include "led_strip.h"
#include "bsp/esp32p4c5_bsp.h"

static const char *TAG = "esp32p4c5_led";
static led_strip_handle_t s_strip;

esp_err_t bsp_led_rgb_init(void)
{
    if (s_strip) {
        return ESP_OK;
    }
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BSP_LED_RGB,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG, "LED strip create failed");
    return led_strip_clear(s_strip);
}

esp_err_t bsp_led_rgb_set(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_ERROR(bsp_led_rgb_init(), TAG, "LED init failed");
    if (red == 0 && green == 0 && blue == 0) {
        return led_strip_clear(s_strip);
    }
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, 0, red, green, blue), TAG, "set pixel failed");
    return led_strip_refresh(s_strip);
}
