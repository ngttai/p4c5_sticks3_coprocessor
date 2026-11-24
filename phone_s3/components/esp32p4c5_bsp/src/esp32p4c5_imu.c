/*
 * LSM6DS3TR-C 6-axis IMU (I2C 0x6A) on the KSDIY ESP32-P4C5 board.
 * Powered from ALDO3, which this BSP does not switch (it stays as the PMIC leaves it).
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp/esp32p4c5_bsp.h"
#include "bsp_err_check.h"

#define IMU_ADDR          0x6A
#define IMU_REG_WHO_AM_I  0x0F
#define IMU_REG_CTRL1_XL  0x10
#define IMU_REG_CTRL2_G   0x11
#define IMU_REG_CTRL3_C   0x12
#define IMU_REG_OUTX_G    0x22   /* gyro X/Y/Z then accel X/Y/Z, 12 bytes, little endian */
#define IMU_WHO_AM_I_VAL  0x6A   /* LSM6DS3TR-C (0x69 is plain LSM6DS3) */

static const char *TAG = "esp32p4c5_imu";
static i2c_master_dev_handle_t s_dev;

static esp_err_t imu_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t bsp_imu_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_ADDR,
        .scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &s_dev), TAG,
                        "add IMU device failed");

    uint8_t who = 0;
    esp_err_t err = i2c_master_transmit_receive(s_dev, (uint8_t[]){IMU_REG_WHO_AM_I}, 1, &who, 1, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU (0x%02X) does not ACK: %s. Check that ALDO3 (IMU rail) is on.", IMU_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    if (who != IMU_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I = 0x%02X, expected 0x%02X (wrong chip or ALDO3 off)", who, IMU_WHO_AM_I_VAL);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* BDU (block data update), register address auto-increment */
    ESP_RETURN_ON_ERROR(imu_write(IMU_REG_CTRL3_C, 0x44), TAG, "CTRL3_C failed");
    /* Accel 104 Hz, +-2 g */
    ESP_RETURN_ON_ERROR(imu_write(IMU_REG_CTRL1_XL, 0x40), TAG, "CTRL1_XL failed");
    /* Gyro 104 Hz, 250 dps */
    ESP_RETURN_ON_ERROR(imu_write(IMU_REG_CTRL2_G, 0x40), TAG, "CTRL2_G failed");

    ESP_LOGI(TAG, "LSM6DS3TR-C ready (WHO_AM_I=0x%02X)", who);
    return ESP_OK;
}

esp_err_t bsp_imu_read(bsp_imu_raw_t *out)
{
    BSP_NULL_CHECK(out, ESP_ERR_INVALID_ARG);
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[12];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, (uint8_t[]){IMU_REG_OUTX_G}, 1, raw, sizeof(raw), 100),
                        TAG, "read output failed");
    for (int i = 0; i < 3; i++) {
        out->gyro[i]  = (int16_t)((raw[2 * i + 1] << 8) | raw[2 * i]);
        out->accel[i] = (int16_t)((raw[6 + 2 * i + 1] << 8) | raw[6 + 2 * i]);
    }
    return ESP_OK;
}
