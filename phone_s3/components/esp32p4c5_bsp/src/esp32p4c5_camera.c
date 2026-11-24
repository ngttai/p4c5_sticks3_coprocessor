/*
 * MIPI CSI camera (SC2336 or OV2710, auto-detected) for the KSDIY ESP32-P4C5 board.
 * Sensor SCCB is on I2C0 (address 0x30). XCLK comes from the on-board 24 MHz oscillator,
 * reset is RC only (no GPIO), so no reset or power-down pin is driven here.
 * Rails: ALDO1 1.8 V and ALDO4 2.9 V, switched by bsp_power_set(BSP_POWER_CAMERA).
 * Ported from the esp-bsp ESP32-P4 Function EV Board BSP.
 */
#include "esp_log.h"
#include "esp_video_init.h"
#include "bsp/esp32p4c5_bsp.h"
#include "bsp_err_check.h"

static const char *TAG = "esp32p4c5_camera";

esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg)
{
    (void)cfg;
    BSP_ERROR_CHECK_RETURN_ERR(bsp_power_set(BSP_POWER_CAMERA, true));
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    const esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bsp_i2c_get_handle(),
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    esp_video_init_config_t cam_config = {
        .csi = &csi_config,
    };
    esp_err_t ret = esp_video_init(&cam_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        (void)bsp_power_set(BSP_POWER_CAMERA, false);
    }
    return ret;
}

esp_err_t bsp_camera_stop(void)
{
    esp_err_t ret = esp_video_deinit();
    BSP_ERROR_CHECK_RETURN_ERR(bsp_power_set(BSP_POWER_CAMERA, false));
    return ret;
}
