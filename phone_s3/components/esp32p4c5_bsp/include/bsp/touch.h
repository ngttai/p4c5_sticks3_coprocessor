/**
 * @file
 * @brief BSP Touchscreen
 *
 * Hardware-level touch API (no LVGL): ST7123 on the board I2C bus.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP touch configuration structure
 *
 * A zero-initialized structure (or NULL passed to bsp_touch_new()) reports coordinates
 * in the panel's native portrait orientation.
 */
typedef struct {
    struct {
        unsigned int swap_xy: 1;   /*!< Swap X and Y after read coordinates */
        unsigned int mirror_x: 1;  /*!< Mirror X after read coordinates */
        unsigned int mirror_y: 1;  /*!< Mirror Y after read coordinates */
    } flags;
} bsp_touch_config_t;

/**
 * @brief Create new touchscreen
 *
 * Initializes the I2C bus if needed. If you want to free resources allocated by this function,
 * you can use bsp_touch_delete().
 *
 * @param[in]  config    touch configuration, NULL for defaults
 * @param[out] ret_touch esp_lcd_touch touchscreen handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd_touch failure
 */
esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch);

/**
 * @brief Deinitialize touchscreen
 */
void bsp_touch_delete(void);

#ifdef __cplusplus
}
#endif
