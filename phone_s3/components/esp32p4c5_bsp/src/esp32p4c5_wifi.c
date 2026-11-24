/*
 * Wi-Fi for the KSDIY ESP32-P4C5 board, S3 variant. The radio is an ESP32-S3 (M5StickS3) wired to
 * JP1, reached over SPI full-duplex through ESP-Hosted 3.1: CLK 26, MOSI 27, MISO 28, CS 29,
 * handshake 30, data ready 45, reset 50 (S3 G9). The on-board ESP32-C5 is held in reset (GPIO54 low)
 * and its SDIO pins stay unused. The host-side configuration (SPI transport, WIFI_REMOTE_LIBRARY_HOSTED)
 * lives in the project's sdkconfig.defaults, not in this BSP.
 */
#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_check.h"
#include "esp_hosted.h"
#include "driver/gpio.h"
#include "bsp/esp32p4c5_bsp.h"
#include "bsp_err_check.h"

static const char *TAG = "esp32p4c5_wifi";
static bool s_wifi_ready;

esp_err_t bsp_wifi_init(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }

    /* NVS is used by the Wi-Fi driver for calibration and config */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    err = esp_netif_init();
    ESP_RETURN_ON_ERROR(err, TAG, "netif init failed");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "create default STA netif failed");
        return ESP_FAIL;
    }

    /* The on-board C5 stays off: EN (GPIO54) low */
    const gpio_config_t c5_en = { .pin_bit_mask = 1ULL << 54, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&c5_en);
    gpio_set_level(54, 0);

    /* Bring the SPI link up first: resets the S3 on GPIO50 and waits for its INIT event */
    ESP_RETURN_ON_FALSE(esp_hosted_connect_to_slave() == ESP_OK, ESP_FAIL, TAG, "S3 not reachable over SPI");

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed (C5 not reachable?)");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    s_wifi_ready = true;
    ESP_LOGI(TAG, "Wi-Fi STA ready (S3 over SPI)");
    return ESP_OK;
}
