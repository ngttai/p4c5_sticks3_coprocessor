/*
 * BSP for the KSDIY ESP32-P4/C5 4.3" board, following esp-bsp conventions.
 *
 * Pin assignment, panel timing and the ST7102 init sequence come from the vendor BSP
 * (kevincoooool/ksdiy_p4c5_bsp v1.3.3) and the board schematic.
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7102.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "axp2101.h"

#include "bsp/esp-bsp.h"
#include "bsp_err_check.h"
#include "bsp_pmic.h"
#include "st7102_init_cmds.h"

static const char *TAG = "esp32p4c5_bsp";

/* Backlight: SY7200 LED driver, active high, 10 kHz PWM */
#define BSP_LCD_BACKLIGHT_LEDC_TIMER    LEDC_TIMER_1
#define BSP_LCD_BACKLIGHT_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BSP_LCD_BACKLIGHT_LEDC_RES      LEDC_TIMER_10_BIT
#define BSP_LCD_BACKLIGHT_LEDC_DUTY_MAX ((1U << 10) - 1U)
#define BSP_LCD_BACKLIGHT_PWM_FREQ_HZ   (10000)

/* Backlight release after the first LVGL frame */
#define BSP_BACKLIGHT_FIRST_FRAME_DELAY_US (40 * 1000)    /* let the DPI scan the new frame out */
#define BSP_BACKLIGHT_FALLBACK_DELAY_US    (3000 * 1000)  /* if the first frame event never arrives */

/* AXP2101 rails */
#define BSP_PMIC_RAIL_RETRIES      (3)
#define BSP_PMIC_RAIL_RETRY_MS     (20)
#define BSP_PMIC_RAIL_SETTLE_MS    (50)
#define BSP_PMIC_ALDO1_CAMERA_V    (1.8f)
#define BSP_PMIC_ALDO4_CAMERA_V    (2.9f)

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
#define BSP_LCD_DPI_COLOR_FORMAT   LCD_COLOR_FMT_RGB888
#else
#define BSP_LCD_DPI_COLOR_FORMAT   LCD_COLOR_FMT_RGB565
#endif

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#if LV_COLOR_DEPTH != BSP_LCD_BITS_PER_PIXEL
#error "LV_COLOR_DEPTH must match the BSP LCD color format (RGB565 = 16, RGB888 = 24)"
#endif
#endif

/**************************************************************************************************
 * MIPI DSI PHY power
 *
 * The PHY LDO (channel 3, 2.5 V) must be on before the I2C bus is used, as in the stock firmware.
 * Both the display path and the PMIC path call this; it is idempotent.
 **************************************************************************************************/

static esp_ldo_channel_handle_t s_dsi_phy_ldo;

static esp_err_t bsp_dsi_phy_power_on(void)
{
    if (s_dsi_phy_ldo != NULL) {
        return ESP_OK;
    }
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    return esp_ldo_acquire_channel(&ldo_config, &s_dsi_phy_ldo);
}

/**************************************************************************************************
 * I2C
 **************************************************************************************************/

static i2c_master_bus_handle_t s_i2c_handle;
static bool s_pmic_initialized;
static uint32_t s_power_mask;

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (s_i2c_handle != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_config, &s_i2c_handle));
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (s_i2c_handle == NULL) {
        return ESP_OK;
    }

    if (s_pmic_initialized) {
        /* The rails keep their state in the PMIC; only the I2C device is removed. */
        (void)axp2101_deinit();
        s_pmic_initialized = false;
    }
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(s_i2c_handle));
    s_i2c_handle = NULL;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return s_i2c_handle;
}

/**************************************************************************************************
 * PMIC rails / power domains
 **************************************************************************************************/

esp_err_t bsp_pmic_init(void)
{
    if (s_pmic_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_dsi_phy_power_on(), TAG, "DSI PHY power failed");
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(axp2101_init(s_i2c_handle), TAG, "AXP2101 init failed");
    esp_err_t ret = axp2101_check_chip_id();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 chip id check failed");
        (void)axp2101_deinit();
        return ret;
    }
    /* DCDC1 is the board's 3.3 V main rail (VCC3V3). It also supplies the 2.2 kOhm pull-ups
     * on the I2C bus, so the bus cannot work without it. The original BSP enabled it at startup;
     * this BSP enables it here so that the PMIC path works without the display being started. */
    esp_err_t rail_err = ESP_FAIL;
    for (int attempt = 0; attempt < BSP_PMIC_RAIL_RETRIES && rail_err != ESP_OK; attempt++) {
        rail_err = axp2101_set_dcdc_voltage(AXP2101_DCDC1, 3.3f);
        if (rail_err == ESP_OK) {
            rail_err = axp2101_set_dcdc_enabled(AXP2101_DCDC1, true);
        }
        if (rail_err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_RAIL_RETRY_MS));
        }
    }
    if (rail_err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 DCDC1 (3.3 V) enable failed: %s", esp_err_to_name(rail_err));
        (void)axp2101_deinit();
        return rail_err;
    }
    vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_RAIL_SETTLE_MS));

    s_pmic_initialized = true;
    return ESP_OK;
}

/* Rail writes are occasionally NACKed by the PMIC, so every write is retried. */
static esp_err_t bsp_pmic_set_ldo(axp2101_ldo_t ldo, float voltage, bool enable)
{
    esp_err_t ret = ESP_OK;
    for (int attempt = 0; attempt < BSP_PMIC_RAIL_RETRIES; attempt++) {
        ret = ESP_OK;
        if (enable) {
            ret = axp2101_set_ldo_voltage(ldo, voltage);
        }
        if (ret == ESP_OK) {
            ret = axp2101_set_ldo_enabled(ldo, enable);
        }
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_RAIL_RETRY_MS));
    }
    ESP_LOGE(TAG, "AXP2101 LDO %d write failed: %s", (int)ldo, esp_err_to_name(ret));
    return ret;
}

/* GPIO-switched domains: active high, level 1 = on. */
#define BSP_POWER_BOOST_5V_GPIO   (GPIO_NUM_20)

static esp_err_t bsp_power_gpio_set(gpio_num_t pin, bool enable)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    BSP_ERROR_CHECK_RETURN_ERR(gpio_config(&cfg));
    BSP_ERROR_CHECK_RETURN_ERR(gpio_set_level(pin, enable ? 1 : 0));
    return ESP_OK;
}


esp_err_t bsp_power_set(bsp_power_domain_t domain, bool enable)
{
    switch (domain) {
    case BSP_POWER_BOOST_5V:
        return bsp_power_gpio_set(BSP_POWER_BOOST_5V_GPIO, enable);
    case BSP_POWER_AUDIO:
    case BSP_POWER_IMU:
        /* ALDO3 is not controlled by this BSP: it feeds the codecs and the IMU, and switching it
         * off leaves the I2C bus unusable. Leave the rail as it is. */
        return ESP_ERR_NOT_SUPPORTED;
    case BSP_POWER_CAMERA:
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "PMIC init failed");

    const uint32_t old_mask = s_power_mask;
    const uint32_t new_mask = enable ? (old_mask | (1U << domain)) : (old_mask & ~(1U << domain));

    if (domain == BSP_POWER_CAMERA) {
        ESP_RETURN_ON_ERROR(bsp_pmic_set_ldo(AXP2101_LDO_ALDO1, BSP_PMIC_ALDO1_CAMERA_V, enable), TAG, "ALDO1 failed");
        esp_err_t ret = bsp_pmic_set_ldo(AXP2101_LDO_ALDO4, BSP_PMIC_ALDO4_CAMERA_V, enable);
        if (ret != ESP_OK) {
            if (enable) {
                /* Roll ALDO1 back so the camera rails are not left half-enabled */
                (void)bsp_pmic_set_ldo(AXP2101_LDO_ALDO1, BSP_PMIC_ALDO1_CAMERA_V, false);
            }
            return ret;
        }
    }

    s_power_mask = new_mask;
    if (enable) {
        vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_RAIL_SETTLE_MS));
    }
    return ESP_OK;
}

/**************************************************************************************************
 * Backlight
 **************************************************************************************************/

static bool s_backlight_initialized;
static portMUX_TYPE s_backlight_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_backlight_deferred;          /* hold brightness requests until the first frame */
static int s_backlight_pending = -1;       /* held brightness request in percent, -1 = none */

static esp_err_t bsp_backlight_apply(int brightness_percent)
{
    const uint32_t duty = (BSP_LCD_BACKLIGHT_LEDC_DUTY_MAX * (uint32_t)brightness_percent) / 100U;
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(BSP_LCD_BACKLIGHT_LEDC_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, duty));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(BSP_LCD_BACKLIGHT_LEDC_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH));
    return ESP_OK;
}

/* On esp_restart() the DSI controller stops refreshing and the panel shows a solid color
 * until the next boot; blank the backlight first. */
static void bsp_backlight_shutdown_handler(void)
{
    (void)ledc_set_duty(BSP_LCD_BACKLIGHT_LEDC_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, 0);
    (void)ledc_update_duty(BSP_LCD_BACKLIGHT_LEDC_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);
}

esp_err_t bsp_display_brightness_init(void)
{
    if (s_backlight_initialized) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = BSP_LCD_BACKLIGHT_LEDC_MODE,
        .duty_resolution = BSP_LCD_BACKLIGHT_LEDC_RES,
        .timer_num = BSP_LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = BSP_LCD_BACKLIGHT_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel_config = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = BSP_LCD_BACKLIGHT_LEDC_MODE,
        .channel = CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&timer_config));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&channel_config));
    (void)esp_register_shutdown_handler(bsp_backlight_shutdown_handler);
    s_backlight_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_display_brightness_deinit(void)
{
    if (!s_backlight_initialized) {
        return ESP_OK;
    }

    (void)esp_unregister_shutdown_handler(bsp_backlight_shutdown_handler);
    s_backlight_initialized = false;
    BSP_ERROR_CHECK_RETURN_ERR(ledc_stop(BSP_LCD_BACKLIGHT_LEDC_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, 0));
    const ledc_timer_config_t timer_config = {
        .speed_mode = BSP_LCD_BACKLIGHT_LEDC_MODE,
        .timer_num = BSP_LCD_BACKLIGHT_LEDC_TIMER,
        .deconfigure = true,
    };
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_pause(BSP_LCD_BACKLIGHT_LEDC_MODE, BSP_LCD_BACKLIGHT_LEDC_TIMER));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&timer_config));

    /* The driver EN pin has a pull-up on the board (R39): a released pin would turn the
     * backlight fully on, so keep it driven low. The level is set before the pin leaves LEDC. */
    BSP_ERROR_CHECK_RETURN_ERR(gpio_set_level(BSP_LCD_BACKLIGHT, 0));
    const gpio_config_t bl_off = {
        .pin_bit_mask = BIT64(BSP_LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    BSP_ERROR_CHECK_RETURN_ERR(gpio_config(&bl_off));
    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (!s_backlight_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    bool held = false;
    portENTER_CRITICAL(&s_backlight_lock);
    if (s_backlight_deferred) {
        s_backlight_pending = brightness_percent;
        held = true;
    }
    portEXIT_CRITICAL(&s_backlight_lock);
    if (held) {
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    return bsp_backlight_apply(brightness_percent);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

/**************************************************************************************************
 * Display (hardware)
 **************************************************************************************************/

static bsp_lcd_handles_t s_lcd_handles;

/* LCD and touch share one reset line. It is only released here; the panel is reset by command.
 * Configured once: display and touch both call this, and a second gpio_config() on the same pin
 * triggers the "conflict found" warning. */
static bool s_lcd_reset_released;

static esp_err_t bsp_lcd_reset_release(void)
{
    if (s_lcd_reset_released) {
        return ESP_OK;
    }
    const gpio_config_t rst_config = {
        .pin_bit_mask = BIT64(BSP_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    BSP_ERROR_CHECK_RETURN_ERR(gpio_config(&rst_config));
    BSP_ERROR_CHECK_RETURN_ERR(gpio_set_level(BSP_LCD_RST, 1));
    s_lcd_reset_released = true;
    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io)
{
    bsp_lcd_handles_t handles = {0};
    BSP_ERROR_CHECK_RETURN_ERR(bsp_display_new_with_handles(config, &handles));

    if (ret_panel) {
        *ret_panel = handles.panel;
    }
    if (ret_io) {
        *ret_io = handles.io;
    }
    return ESP_OK;
}

esp_err_t bsp_display_new_with_handles(const bsp_display_config_t *config, bsp_lcd_handles_t *ret_handles)
{
    esp_err_t ret = ESP_OK;
    const bsp_display_config_t default_config = {0};
    if (config == NULL) {
        config = &default_config;
    }
    if (s_lcd_handles.panel != NULL) {
        ESP_LOGE(TAG, "Display is already created");
        return ESP_ERR_INVALID_STATE;
    }

    BSP_ERROR_CHECK_RETURN_ERR(bsp_display_brightness_init());
    BSP_ERROR_CHECK_RETURN_ERR(bsp_lcd_reset_release());

    /* MIPI DSI PHY is powered from the internal LDO */
    ESP_RETURN_ON_ERROR(bsp_dsi_phy_power_on(), TAG, "DSI PHY power failed");

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = config->dsi_bus.phy_clk_src,
        .lane_bit_rate_mbps = config->dsi_bus.lane_bit_rate_mbps ?
        config->dsi_bus.lane_bit_rate_mbps : BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), err, TAG, "New DSI bus init failed");

    const esp_lcd_dbi_io_config_t dbi_config = ST7102_MIPI_PANEL_IO_DBI_CONFIG();
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io), err, TAG, "New panel IO failed");

    esp_lcd_dpi_panel_config_t dpi_config = ST7102_MIPI_480_800_PANEL_60HZ_DPI_CONFIG(BSP_LCD_DPI_COLOR_FORMAT);
    dpi_config.num_fbs = config->dpi.num_fbs ? config->dpi.num_fbs : 1;

    const st7102_vendor_config_t vendor_config = {
        .init_cmds = bsp_st7102_init_cmds,
        .init_cmds_size = sizeof(bsp_st7102_init_cmds) / sizeof(bsp_st7102_init_cmds[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7102(io, &panel_config, &panel), err, TAG, "New LCD panel ST7102 failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(panel), err, TAG, "LCD panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(panel), err, TAG, "LCD panel init failed");

    s_lcd_handles.mipi_dsi_bus = mipi_dsi_bus;
    s_lcd_handles.io = io;
    s_lcd_handles.panel = panel;
    if (ret_handles) {
        *ret_handles = s_lcd_handles;
    }
    ESP_LOGI(TAG, "Display initialized: %dx%d, %d bpp", BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_BITS_PER_PIXEL);
    return ESP_OK;

err:
    if (panel) {
        esp_lcd_panel_del(panel);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    if (mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(mipi_dsi_bus);
    }
    if (s_dsi_phy_ldo) {
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
    }
    BSP_ERROR_CHECK_RETURN_ERR(ret);
    return ret;
}

void bsp_display_delete(void)
{
    if (s_lcd_handles.panel) {
        esp_lcd_panel_del(s_lcd_handles.panel);
    }
    if (s_lcd_handles.io) {
        esp_lcd_panel_io_del(s_lcd_handles.io);
    }
    if (s_lcd_handles.mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(s_lcd_handles.mipi_dsi_bus);
    }
    s_lcd_handles = (bsp_lcd_handles_t) {
        0
    };
    if (s_dsi_phy_ldo) {
        esp_ldo_release_channel(s_dsi_phy_ldo);
        s_dsi_phy_ldo = NULL;
    }
}

/**************************************************************************************************
 * Touch
 **************************************************************************************************/

static esp_lcd_touch_handle_t s_touch_handle;
static esp_lcd_panel_io_handle_t s_touch_io_handle;

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = ESP_OK;
    const bsp_touch_config_t default_config = {0};
    if (config == NULL) {
        config = &default_config;
    }
    if (s_touch_handle != NULL) {
        ESP_LOGE(TAG, "Touch is already created");
        return ESP_ERR_INVALID_STATE;
    }

    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
    BSP_ERROR_CHECK_RETURN_ERR(bsp_lcd_reset_release());

    const esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = config->flags.swap_xy,
            .mirror_x = config->flags.mirror_x,
            .mirror_y = config->flags.mirror_y,
        },
    };
    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_handle, &io_config, &s_touch_io_handle), err, TAG, "New touch IO failed");
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_st7123(s_touch_io_handle, &touch_config, &s_touch_handle), err, TAG, "New touch ST7123 failed");

    if (ret_touch) {
        *ret_touch = s_touch_handle;
    }
    return ESP_OK;

err:
    bsp_touch_delete();
    BSP_ERROR_CHECK_RETURN_ERR(ret);
    return ret;
}

void bsp_touch_delete(void)
{
    if (s_touch_handle) {
        esp_lcd_touch_del(s_touch_handle);
        s_touch_handle = NULL;
    }
    if (s_touch_io_handle) {
        esp_lcd_panel_io_del(s_touch_io_handle);
        s_touch_io_handle = NULL;
    }
}

/**************************************************************************************************
 * Display (LVGL)
 **************************************************************************************************/

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

static lv_display_t *s_display;
static lv_indev_t *s_touch_indev;
static esp_timer_handle_t s_backlight_timer;
static bool s_first_frame_seen;

/* Stop holding brightness requests and apply the held one, if any. */
static void bsp_backlight_release_timer_cb(void *arg)
{
    (void)arg;
    int pending;
    portENTER_CRITICAL(&s_backlight_lock);
    s_backlight_deferred = false;
    pending = s_backlight_pending;
    s_backlight_pending = -1;
    portEXIT_CRITICAL(&s_backlight_lock);

    if (pending >= 0 && s_backlight_initialized) {
        (void)bsp_backlight_apply(pending);
    }
}

/* LV_EVENT_REFR_READY means the frame is rendered and flushed, not that the panel shows it yet. */
static void bsp_display_first_frame_cb(lv_event_t *e)
{
    (void)e;
    if (s_first_frame_seen) {
        return;
    }
    s_first_frame_seen = true;
    (void)esp_timer_stop(s_backlight_timer);
    if (esp_timer_start_once(s_backlight_timer, BSP_BACKLIGHT_FIRST_FRAME_DELAY_US) != ESP_OK) {
        bsp_backlight_release_timer_cb(NULL);
    }
}

static bsp_touch_config_t bsp_touch_config_for_rotation(esp_lv_adapter_rotation_t rotation)
{
    bsp_touch_config_t config = {0};
    switch (rotation) {
    case ESP_LV_ADAPTER_ROTATE_90:
        config.flags.swap_xy = 1;
        config.flags.mirror_x = 1;
        break;
    case ESP_LV_ADAPTER_ROTATE_180:
        config.flags.mirror_x = 1;
        config.flags.mirror_y = 1;
        break;
    case ESP_LV_ADAPTER_ROTATE_270:
        config.flags.swap_xy = 1;
        config.flags.mirror_y = 1;
        break;
    case ESP_LV_ADAPTER_ROTATE_0:
    default:
        break;
    }
    return config;
}

lv_display_t *bsp_display_start(void)
{
    const bsp_display_cfg_t cfg = BSP_DISPLAY_CFG_DEFAULT();
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    if (cfg == NULL) {
        ESP_LOGE(TAG, "display config is NULL");
        return NULL;
    }
    if (s_display != NULL) {
        return s_display;
    }

    /* The adapter needs one DPI frame buffer per tear-avoidance buffer. */
    bsp_display_config_t hw_cfg = cfg->hw_cfg;
    if (hw_cfg.dpi.num_fbs == 0) {
        hw_cfg.dpi.num_fbs = esp_lv_adapter_get_required_frame_buffer_count(cfg->tear_avoid_mode, cfg->rotation);
    }

    /* Each step below unwinds the steps before it on failure; never abort, return NULL. */
    esp_err_t err;
    bsp_lcd_handles_t lcd = {0};
    err = bsp_display_new_with_handles(&hw_cfg, &lcd);          /* cleans its own steps on error */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return NULL;
    }
    err = esp_lcd_panel_disp_on_off(lcd.panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel on failed: %s", esp_err_to_name(err));
        goto fail_lcd;
    }

    esp_lcd_touch_handle_t touch = NULL;
    const bsp_touch_config_t touch_hw_cfg = bsp_touch_config_for_rotation(cfg->rotation);
    err = bsp_touch_new(&touch_hw_cfg, &touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(err));
        goto fail_lcd;
    }

    err = esp_lv_adapter_init(&cfg->lvgl_adapter_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter init failed: %s", esp_err_to_name(err));
        goto fail_touch;
    }

    esp_lv_adapter_display_config_t display_config = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
                                                         lcd.panel, lcd.io, BSP_LCD_H_RES, BSP_LCD_V_RES, cfg->rotation);
    display_config.tear_avoid_mode = cfg->tear_avoid_mode;
    s_display = esp_lv_adapter_register_display(&display_config);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "register display failed");
        goto fail_adapter;
    }

    const esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_display, touch);
    s_touch_indev = esp_lv_adapter_register_touch(&touch_config);
    if (s_touch_indev == NULL) {
        ESP_LOGE(TAG, "register touch failed");
        goto fail_display;
    }

    /* Hold the backlight until the first frame is on the panel, so that uninitialized
     * frame buffer content is never visible. The LVGL task is not running yet, so the
     * event callback can be added without taking the LVGL lock. */
    const esp_timer_create_args_t timer_args = {
        .callback = bsp_backlight_release_timer_cb,
        .name = "bsp_backlight",
    };
    err = esp_timer_create(&timer_args, &s_backlight_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight timer create failed: %s", esp_err_to_name(err));
        goto fail_touch_indev;
    }
    s_first_frame_seen = false;
    portENTER_CRITICAL(&s_backlight_lock);
    s_backlight_deferred = true;
    s_backlight_pending = -1;
    portEXIT_CRITICAL(&s_backlight_lock);
    lv_display_add_event_cb(s_display, bsp_display_first_frame_cb, LV_EVENT_REFR_READY, NULL);

    /* Arm the fallback before LVGL starts: the first frame callback re-arms this timer. */
    err = esp_timer_start_once(s_backlight_timer, BSP_BACKLIGHT_FALLBACK_DELAY_US);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight timer start failed: %s", esp_err_to_name(err));
        goto fail_timer;
    }
    err = esp_lv_adapter_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL adapter start failed: %s", esp_err_to_name(err));
        goto fail_timer;
    }

    return s_display;

fail_timer:
    (void)esp_timer_stop(s_backlight_timer);
    (void)esp_timer_delete(s_backlight_timer);
    s_backlight_timer = NULL;
    portENTER_CRITICAL(&s_backlight_lock);
    s_backlight_deferred = false;
    s_backlight_pending = -1;
    portEXIT_CRITICAL(&s_backlight_lock);
fail_touch_indev:
    (void)esp_lv_adapter_unregister_touch(s_touch_indev);
    s_touch_indev = NULL;
fail_display:
    (void)esp_lv_adapter_unregister_display(s_display);
    s_display = NULL;
fail_adapter:
    (void)esp_lv_adapter_deinit();
fail_touch:
    bsp_touch_delete();
fail_lcd:
    bsp_display_delete();
    return NULL;
}

void bsp_display_stop(lv_display_t *display)
{
    if (display == NULL || display != s_display) {
        return;
    }

    (void)bsp_backlight_apply(0);
    if (s_backlight_timer) {
        (void)esp_timer_stop(s_backlight_timer);
        (void)esp_timer_delete(s_backlight_timer);
        s_backlight_timer = NULL;
    }
    portENTER_CRITICAL(&s_backlight_lock);
    s_backlight_deferred = false;
    s_backlight_pending = -1;
    portEXIT_CRITICAL(&s_backlight_lock);

    if (s_touch_indev) {
        (void)esp_lv_adapter_unregister_touch(s_touch_indev);
        s_touch_indev = NULL;
    }
    (void)esp_lv_adapter_unregister_display(s_display);
    s_display = NULL;
    (void)esp_lv_adapter_deinit();

    bsp_touch_delete();
    bsp_display_delete();
    (void)bsp_display_brightness_deinit();
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return s_touch_indev;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    /* esp-bsp contract: 0 blocks indefinitely; the adapter uses -1 for that. */
    return esp_lv_adapter_lock(timeout_ms == 0 ? -1 : (int32_t)timeout_ms) == ESP_OK;
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}

#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0
