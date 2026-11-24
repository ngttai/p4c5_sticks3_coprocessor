/**
 * @file
 * @brief BSP LCD
 *
 * Hardware-level display API (no LVGL): MIPI DSI bus, ST7102 panel and backlight.
 * For the LVGL API see bsp/esp32p4c5_bsp.h (bsp_display_start()).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"
#include "sdkconfig.h"

/* LCD color formats */
#define ESP_LCD_COLOR_FORMAT_RGB565    (1)
#define ESP_LCD_COLOR_FORMAT_RGB888    (2)

/* LCD display color format and bits per pixel */
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#define BSP_LCD_COLOR_FORMAT        (ESP_LCD_COLOR_FORMAT_RGB888)
#define BSP_LCD_BITS_PER_PIXEL      (24)
#else
#define BSP_LCD_COLOR_FORMAT        (ESP_LCD_COLOR_FORMAT_RGB565)
#define BSP_LCD_BITS_PER_PIXEL      (16)
#endif
/* LCD display color bytes endianness */
#define BSP_LCD_BIGENDIAN           (0)
/* LCD display color space */
#define BSP_LCD_COLOR_SPACE         (LCD_RGB_ELEMENT_ORDER_RGB)

/* LCD definition: 4.3" ST7102, portrait */
#define BSP_LCD_H_RES               (480)
#define BSP_LCD_V_RES               (800)

#define BSP_LCD_MIPI_DSI_LANE_NUM          (2)    // 2 data lanes
#define BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS (820)  // per lane

#define BSP_MIPI_DSI_PHY_PWR_LDO_CHAN       (3)   // LDO_VO3 is connected to VDD_MIPI_DPHY
#define BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV (2500)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP display configuration structure
 *
 * A zero-initialized structure (or NULL passed to bsp_display_new()) selects the board defaults.
 */
typedef struct {
    struct {
        mipi_dsi_phy_clock_source_t phy_clk_src;  /*!< DSI bus config - clock source. 0 lets esp_lcd pick the
                                                       source that fits the chip revision; do not use
                                                       MIPI_DSI_PHY_CLK_SRC_DEFAULT, it aborts on P4 rev v3.x */
        uint32_t lane_bit_rate_mbps;              /*!< DSI bus config - lane bit rate, 0 = board default */
    } dsi_bus;
    struct {
        uint8_t num_fbs;                          /*!< Number of DPI frame buffers, 0 = 1 */
    } dpi;
} bsp_display_config_t;

/**
 * @brief BSP display return handles
 */
typedef struct {
    esp_lcd_dsi_bus_handle_t    mipi_dsi_bus;  /*!< MIPI DSI bus handle */
    esp_lcd_panel_io_handle_t   io;            /*!< ESP LCD IO handle */
    esp_lcd_panel_handle_t      panel;         /*!< ESP LCD panel (color) handle */
} bsp_lcd_handles_t;

/**
 * @brief Create new display panel
 *
 * For maximum flexibility, this function performs only reset and initialization of the display.
 * You must turn on the display explicitly by calling esp_lcd_panel_disp_on_off().
 * The display's backlight is not turned on either. You can use bsp_display_backlight_on/off(),
 * bsp_display_brightness_set() or implement your own backlight control.
 *
 * If you want to free resources allocated by this function, you can use bsp_display_delete().
 *
 * @param[in]  config    display configuration, NULL for board defaults
 * @param[out] ret_panel esp_lcd panel handle
 * @param[out] ret_io    esp_lcd IO handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd failure
 */
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io);

/**
 * @brief Create new display panel, returning all handles
 *
 * Same as bsp_display_new(), but also returns the MIPI DSI bus handle.
 *
 * @param[in]  config      display configuration, NULL for board defaults
 * @param[out] ret_handles all esp_lcd handles in one structure
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd failure
 */
esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles);

/**
 * @brief Delete display panel
 *
 * Releases the panel, panel IO, DSI bus and the MIPI PHY LDO acquired by bsp_display_new().
 */
void bsp_display_delete(void);

/**
 * @brief Initialize display's brightness
 *
 * Brightness is controlled with PWM signal to a pin controlling backlight.
 * The backlight stays off after this call. It is called from bsp_display_new().
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Parameter error
 */
esp_err_t bsp_display_brightness_init(void);

/**
 * @brief Deinitialize display's brightness
 *
 * Releases the PWM channel and timer. The backlight pin stays driven low (backlight off),
 * because the board pulls the driver enable pin up.
 *
 * @return
 *      - ESP_OK                On success
 */
esp_err_t bsp_display_brightness_deinit(void);

/**
 * @brief Set display's brightness
 *
 * Brightness must be already initialized by calling bsp_display_brightness_init() or bsp_display_new().
 *
 * After bsp_display_start(), a value set before LVGL has flushed its first frame is applied right
 * after that frame (so the panel never shows uninitialized frame buffer content).
 *
 * @param[in] brightness_percent Brightness in [%]
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE Brightness is not initialized
 */
esp_err_t bsp_display_brightness_set(int brightness_percent);

/**
 * @brief Turn on display backlight
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE Brightness is not initialized
 */
esp_err_t bsp_display_backlight_on(void);

/**
 * @brief Turn off display backlight
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE Brightness is not initialized
 */
esp_err_t bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif
