/*
 * Shared between the BSP sources: AXP2101 bring-up (I2C bus, chip id, DCDC1 3.3 V main rail).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the I2C bus and the AXP2101 once; later calls return ESP_OK. */
esp_err_t bsp_pmic_init(void);

#ifdef __cplusplus
}
#endif
