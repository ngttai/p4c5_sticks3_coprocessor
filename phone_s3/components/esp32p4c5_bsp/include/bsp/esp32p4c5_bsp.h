/**
 * @file
 * @brief ESP BSP: KSDIY ESP32-P4/C5 4.3" board
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "sd_protocol_types.h"
#include "iot_button.h"
#include "bsp/config.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
#include "lvgl.h"
#include "esp_lv_adapter.h"
#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

/**************************************************************************************************
 *  BSP Board Name
 **************************************************************************************************/

#define BSP_BOARD_KSDIY_P4C5

/**************************************************************************************************
 *  BSP Capabilities
 *
 *  The board also carries audio codecs (ES8311/ES7210), a microSD slot, an IMU (LSM6DS3TR-C),
 *  a CSI camera connector and an ESP32-C5 radio. This BSP does not drive them yet, so their
 *  capabilities are reported as 0.
 **************************************************************************************************/

#define BSP_CAPS_DISPLAY        1
#define BSP_CAPS_TOUCH          1
#define BSP_CAPS_BUTTONS        1
#define BSP_CAPS_AUDIO          1
#define BSP_CAPS_AUDIO_SPEAKER  1
#define BSP_CAPS_AUDIO_MIC      1
#define BSP_CAPS_SDCARD         1
#define BSP_CAPS_WIFI           1
#define BSP_CAPS_IMU            1
#define BSP_CAPS_CAMERA         1
#define BSP_CAPS_BATTERY        1
#define BSP_CAPS_LED            1

/**************************************************************************************************
 *  KSDIY ESP32-P4/C5 pinout
 **************************************************************************************************/
/* I2C (touch, PMIC, codecs, IMU, camera SCCB) */
#define BSP_I2C_SCL           (GPIO_NUM_8)
#define BSP_I2C_SDA           (GPIO_NUM_7)

/* Display */
#define BSP_LCD_BACKLIGHT     (GPIO_NUM_6)
#define BSP_LCD_RST           (GPIO_NUM_22)   // Shared by LCD and touch, has an external pull-up
#define BSP_LCD_TOUCH_RST     (GPIO_NUM_NC)   // Shared with BSP_LCD_RST
#define BSP_LCD_TOUCH_INT     (GPIO_NUM_23)

/* I2S0 audio data: MCLK 13, BCLK 12, WS 10, DOUT 9 (P4 -> ES8311), DIN 11 (ES7210 -> P4) */
#define BSP_I2S_NUM           (I2S_NUM_0)
#define BSP_I2S_MCLK          (GPIO_NUM_13)
#define BSP_I2S_BCLK          (GPIO_NUM_12)
#define BSP_I2S_WS            (GPIO_NUM_10)
#define BSP_I2S_DOUT          (GPIO_NUM_9)
#define BSP_I2S_DIN           (GPIO_NUM_11)
/* Speaker amplifier enable (NS4150B STD), active high */
#define BSP_SPEAKER_PA        (GPIO_NUM_3)

/* Buttons, active low with 10 kOhm pull-ups (the power key is on the PMIC) */
#define BSP_BUTTON_BOOT_IO    (GPIO_NUM_35)   // Also the boot-mode strap, driven by the CH343P DTR line
#define BSP_BUTTON_IO0_IO     (GPIO_NUM_0)    // Also a P4 strapping pin

/* RGB LED (WS2812B) */
#define BSP_LED_RGB           (GPIO_NUM_34)


/* PMIC (AXP2101) */
#define BSP_PMIC_IRQ          (GPIO_NUM_21)

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************************
 *
 * I2C interface
 *
 * There are multiple devices connected to I2C peripheral:
 *  - LCD Touch controller ST7123 (0x55)
 *  - PMIC AXP2101 (0x34)
 *  - Codec ES8311 (0x18), audio ADC ES7210 (0x40)
 *  - IMU LSM6DS3TR-C (0x6A)
 *  - Camera SCCB (0x30)
 **************************************************************************************************/
#define BSP_I2C_NUM     CONFIG_BSP_I2C_NUM

/**
 * @brief Init I2C driver
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   I2C parameter error
 *      - ESP_FAIL              I2C driver installation error
 */
esp_err_t bsp_i2c_init(void);

/**
 * @brief Deinit I2C driver and free its resources
 *
 * Devices created on the bus (touch, PMIC) must be deleted first; the PMIC device
 * owned by the BSP is removed by this call.
 *
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   I2C parameter error
 */
esp_err_t bsp_i2c_deinit(void);

/**
 * @brief Get I2C driver handle
 *
 * @return
 *      - I2C handle, NULL if bsp_i2c_init() was not called
 */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/**************************************************************************************************
 *
 * BSP Power Domains
 *
 * bsp_power_set() switches the camera rails (ALDO1 1.8 V + ALDO4 2.9 V) and the boost GPIO.
 * Audio and IMU return ESP_ERR_NOT_SUPPORTED: ALDO3 is left as it is.
 **************************************************************************************************/
typedef enum {
    BSP_POWER_AUDIO,      /* AXP2101 ALDO3. Not controlled by the BSP (returns ESP_ERR_NOT_SUPPORTED) */
    BSP_POWER_IMU,        /* AXP2101 ALDO3. Not controlled by the BSP (returns ESP_ERR_NOT_SUPPORTED) */
    BSP_POWER_CAMERA,     /* AXP2101 ALDO1 + ALDO4 */
    BSP_POWER_BOOST_5V,   /* SY7088 5 V boost, GPIO20 (BOOST_ON), active high. Powers USB-A VBUS, RS485, MCP4725 */
} bsp_power_domain_t;

/**
 * @brief Enable or disable a power domain
 *
 * Initializes the I2C bus and the PMIC on first use. A PMIC communication failure is always
 * returned to the caller, it never asserts.
 *
 * @param[in] domain Power domain to enable or disable
 * @param[in] enable true to power the domain, false to release it
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_ARG   Unknown power domain
 *      - Else                  I2C or PMIC failure
 */
esp_err_t bsp_power_set(bsp_power_domain_t domain, bool enable);

/**************************************************************************************************
 *
 * Battery, charger and power key (AXP2101)
 *
 * Battery on J2 (with NTC), charge LED driven by the PMIC, power key BTN1 on PWRON,
 * PMIC IRQ on GPIO21. The charger settings are left as they are unless
 * CONFIG_BSP_PMIC_SET_CHARGER is enabled.
 **************************************************************************************************/

typedef enum {
    BSP_BATTERY_ABSENT = 0,     /*!< No battery detected */
    BSP_BATTERY_DISCHARGING,    /*!< Battery present, not charging */
    BSP_BATTERY_CHARGING,       /*!< Pre-charge, constant current or constant voltage phase */
    BSP_BATTERY_FULL,           /*!< Charge done */
} bsp_battery_state_t;

typedef struct {
    bsp_battery_state_t state;
    int percent;                /*!< Fuel gauge state of charge 0..100, -1 without battery */
    uint16_t voltage_mv;        /*!< Battery voltage, 0 without battery */
    bool vbus_present;          /*!< USB supply present and good */
    uint16_t vbus_mv;           /*!< USB supply voltage, 0 without USB */
    uint16_t vsys_mv;           /*!< System rail voltage */
    float die_temp_c;           /*!< AXP2101 die temperature */
    uint16_t charge_current_ma; /*!< Constant charge current programmed in the charger */
    uint16_t charge_voltage_mv; /*!< Charge target voltage programmed in the charger */
} bsp_battery_info_t;

/**
 * @brief Enable the PMIC measurements (battery, USB and system voltage, die temperature, fuel gauge)
 *
 * Also programs the charger when CONFIG_BSP_PMIC_SET_CHARGER is set. Called by the other
 * battery functions, so calling it first is optional.
 *
 * @return ESP_OK on success, else I2C or PMIC failure
 */
esp_err_t bsp_battery_init(void);

/**
 * @brief Read the battery, USB supply and charger state
 *
 * @param[out] info Filled with the current values
 * @return ESP_OK on success, else I2C or PMIC failure
 */
esp_err_t bsp_battery_get_info(bsp_battery_info_t *info);

typedef enum {
    BSP_PMIC_EVENT_KEY_SHORT_PRESS,   /*!< Power key short press */
    BSP_PMIC_EVENT_KEY_LONG_PRESS,    /*!< Power key held (before the PMIC hard power-off time) */
    BSP_PMIC_EVENT_VBUS_INSERTED,     /*!< USB supply connected */
    BSP_PMIC_EVENT_VBUS_REMOVED,      /*!< USB supply disconnected */
    BSP_PMIC_EVENT_BATTERY_INSERTED,
    BSP_PMIC_EVENT_BATTERY_REMOVED,
    BSP_PMIC_EVENT_CHARGE_START,
    BSP_PMIC_EVENT_CHARGE_DONE,
    BSP_PMIC_EVENT_BATTERY_LOW,       /*!< Fuel gauge reached a low battery warning level */
} bsp_pmic_event_t;

/**
 * @brief PMIC event callback, called from a BSP task (not from an ISR)
 *
 * Do not block in it; take the LVGL lock before touching the UI.
 */
typedef void (*bsp_pmic_event_cb_t)(bsp_pmic_event_t event, void *arg);

/**
 * @brief Report PMIC events (power key, USB supply, battery, charger) through a callback
 *
 * Starts the IRQ handling task on the first call. Only one callback is supported.
 *
 * @param[in] cb  Callback
 * @param[in] arg Passed to the callback
 * @return
 *      - ESP_OK                On success
 *      - ESP_ERR_INVALID_STATE A callback is already registered
 *      - Else                  I2C, PMIC or GPIO failure
 */
esp_err_t bsp_pmic_event_register(bsp_pmic_event_cb_t cb, void *arg);

/**
 * @brief Switch the board off through the PMIC
 *
 * Turns the backlight off, then asks the AXP2101 to cut its outputs. The power key turns
 * the board on again.
 *
 * @return Does not return on success; ESP_FAIL if the board is still running afterwards
 */
esp_err_t bsp_power_off(void);

/**************************************************************************************************
 *
 * LCD interface
 *
 * 4.3" 480x800 MIPI DSI panel (ST7102) with capacitive touch (ST7123).
 * LVGL is run through esp_lvgl_adapter, which provides the tear-free frame buffer handling.
 *
 * After bsp_display_start() the backlight is off: call bsp_display_backlight_on() or
 * bsp_display_brightness_set() once the first screen is ready.
 **************************************************************************************************/

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

/**
 * @brief BSP display configuration structure
 *
 * Use BSP_DISPLAY_CFG_DEFAULT() and override what you need.
 */
typedef struct {
    esp_lv_adapter_config_t          lvgl_adapter_cfg; /*!< LVGL adapter (task, tick) configuration */
    bsp_display_config_t             hw_cfg;           /*!< Display HW configuration */
    esp_lv_adapter_rotation_t        rotation;         /*!< Display and touch rotation. It is fixed for the
                                                            lifetime of the display: the frame buffer layout
                                                            depends on it */
    esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode;  /*!< Tearing avoidance mode */
} bsp_display_cfg_t;

/**
 * @brief Default display configuration: portrait, adapter's default MIPI DSI tear avoidance
 */
#define BSP_DISPLAY_CFG_DEFAULT() {                                                \
        .lvgl_adapter_cfg = bsp_display_lvgl_adapter_default_cfg(),                \
        .hw_cfg = { .dsi_bus = { .lane_bit_rate_mbps = 0 } },                                                       \
        .rotation = ESP_LV_ADAPTER_ROTATE_0,                                       \
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_MIPI_DSI,        \
    }

/**
 * @brief Adapter configuration used by BSP_DISPLAY_CFG_DEFAULT()
 */
static inline esp_lv_adapter_config_t bsp_display_lvgl_adapter_default_cfg(void)
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_stack_size = CONFIG_BSP_DISPLAY_LVGL_TASK_STACK_SIZE;
    return cfg;
}

/**
 * @brief Initialize display
 *
 * This function initializes the MIPI DSI panel, the touch controller and the LVGL adapter.
 * When the function returns, LVGL is running; the backlight is still off.
 *
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start(void);

/**
 * @brief Initialize display with a custom configuration
 *
 * @param[in] cfg display configuration
 * @return Pointer to LVGL display or NULL when error occurred
 */
lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg);

/**
 * @brief Deinitialize display
 *
 * Stops LVGL and releases the touch controller, the panel and the backlight.
 * The I2C bus is left initialized.
 *
 * @param[in] display Pointer returned by bsp_display_start()
 */
void bsp_display_stop(lv_display_t *display);

/**
 * @brief Get pointer to input device (touch)
 *
 * @note The LVGL input device is initialized in bsp_display_start() function.
 *
 * @return Pointer to LVGL input device or NULL when not initialized
 */
lv_indev_t *bsp_display_get_input_dev(void);

/**
 * @brief Take LVGL mutex
 *
 * @param[in] timeout_ms Timeout in [ms]. 0 will block indefinitely.
 * @return true  Mutex was taken
 * @return false Mutex was NOT taken
 */
bool bsp_display_lock(uint32_t timeout_ms);

/**
 * @brief Give LVGL mutex
 */
void bsp_display_unlock(void);

#endif // BSP_CONFIG_NO_GRAPHIC_LIB == 0

/**************************************************************************************************
 *
 * microSD card (SDMMC slot 0, 4-bit, FAT via VFS)
 *
 **************************************************************************************************/

/**
 * @brief Power the microSD card and mount FAT at CONFIG_BSP_SD_MOUNT_POINT
 *
 * @return ESP_OK on success, otherwise the SDMMC / FAT / power error
 */
esp_err_t bsp_sdcard_mount(void);

/**
 * @brief Unmount the microSD card and remove its power control
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_sdcard_unmount(void);

/**
 * @brief Power and initialize the microSD card without mounting a filesystem
 *
 * For raw sector access, for example a USB mass storage device. The card must not be mounted;
 * release it with bsp_sdcard_raw_deinit() before calling bsp_sdcard_mount() again.
 *
 * @param[out] card Initialized card, owned by the BSP
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if mounted or already initialized
 */
esp_err_t bsp_sdcard_raw_init(sdmmc_card_t **card);

/**
 * @brief Release the card of bsp_sdcard_raw_init()
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_sdcard_raw_deinit(void);

/**************************************************************************************************
 *
 * Buttons: BOOT (GPIO35) and IO0 (GPIO0), active low with external pull-ups
 *
 * The power key (BTN1) is on the AXP2101 PWRON pin, see bsp_pmic_event_register().
 *
 * Example:
 * \code{.c}
 * button_handle_t btns[BSP_BUTTON_NUM];
 * bsp_iot_button_create(btns, NULL, BSP_BUTTON_NUM);
 * iot_button_register_cb(btns[BSP_BUTTON_BOOT], BUTTON_PRESS_DOWN, NULL, on_press, NULL);
 * \endcode
 **************************************************************************************************/

typedef enum {
    BSP_BUTTON_BOOT = 0,
    BSP_BUTTON_IO0,
    BSP_BUTTON_NUM
} bsp_button_t;

/**
 * @brief Initialize all buttons
 *
 * Returned button handlers must be used with espressif/button component API
 *
 * @param[out] btn_array      Output button array
 * @param[out] btn_cnt        Number of button handlers saved to btn_array, can be NULL
 * @param[in]  btn_array_size Size of output button array. Must be at least BSP_BUTTON_NUM
 * @return
 *     - ESP_OK               All buttons initialized
 *     - ESP_ERR_INVALID_ARG  btn_array is too small or NULL
 *     - ESP_FAIL             Underlying iot_button_new_gpio_device failed
 */
esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size);

/**************************************************************************************************
 *
 * IMU: LSM6DS3TR-C (I2C 0x6A). Raw 16-bit output.
 * Accel +-2 g: 0.061 mg/LSB. Gyro 250 dps: 8.75 mdps/LSB.
 *
 **************************************************************************************************/

typedef struct {
    int16_t gyro[3];    /*!< X, Y, Z raw */
    int16_t accel[3];   /*!< X, Y, Z raw */
} bsp_imu_raw_t;

/**
 * @brief Detect and configure the IMU (WHO_AM_I must read 0x6A (LSM6DS3TR-C))
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the IMU does not answer with the right ID
 */
esp_err_t bsp_imu_init(void);

/**
 * @brief Read one gyro and accel sample
 *
 * @param[out] out raw sample
 * @return ESP_OK on success
 */
esp_err_t bsp_imu_read(bsp_imu_raw_t *out);

/**************************************************************************************************
 *
 * Audio: ES8311 DAC/speaker and ES7210 ADC/mics (I2C0 + I2S0), via esp_codec_dev
 *
 **************************************************************************************************/

/** I2S0 pins of the codecs */
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_BCLK,  \
        .ws = BSP_I2S_WS,      \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DIN,    \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

/** Mono, 16-bit, full duplex I2S configuration, used by bsp_audio_init(NULL) */
#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

/**
 * @brief Init the I2S0 channels shared by the speaker and the microphones
 *
 * Called by bsp_audio_codec_speaker_init() and bsp_audio_codec_microphone_init(); call it first
 * only to pass a custom configuration. TX and RX share the clock, so playback and recording
 * always run at the same sample rate.
 *
 * @param[in] i2s_config I2S configuration, NULL for BSP_I2S_DUPLEX_MONO_CFG(22050)
 * @return ESP_OK on success, else the I2S or esp_codec_dev error
 */
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);

/** S3 audio link counters since start: I2S frames received, lost/out-of-order frames, playback underruns, mic overruns. */
void bsp_audio_link_stats(unsigned *frames, unsigned *counter_errors, unsigned *play_underruns, unsigned *mic_overruns);

/**
 * @brief Get the speaker device (ES8311 + I2S0 + PA on GPIO3), created on first call
 *
 * @return esp_codec_dev handle, NULL on failure. Use esp_codec_dev_open/write/close.
 */
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

/**
 * @brief Get the microphone device (ES7210 + I2S0, MIC1 and MIC2), created on first call
 *
 * @return esp_codec_dev handle, NULL on failure. Use esp_codec_dev_open/read/close.
 */
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);

/**************************************************************************************************
 *
 * Camera: MIPI CSI sensor (SC2336 or OV2710), via esp_video
 *
 **************************************************************************************************/

typedef struct {
    uint8_t dummy;  /*!< Reserved for future use */
} bsp_camera_cfg_t;

/**
 * @brief Power the camera rails and initialize esp_video with the CSI sensor
 *
 * After this call the sensor is available through V4L2 on ESP_VIDEO_MIPI_CSI_DEVICE_NAME.
 *
 * @param[in] cfg camera configuration (unused for now), may be NULL
 * @return ESP_OK on success
 */
esp_err_t bsp_camera_start(const bsp_camera_cfg_t *cfg);

/**
 * @brief Deinitialize esp_video and power off the camera rails
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_camera_stop(void);

/**************************************************************************************************
 *
 * SPIFFS on the "storage" partition
 *
 **************************************************************************************************/

#define BSP_SPIFFS_MOUNT_POINT CONFIG_BSP_SPIFFS_MOUNT_POINT

/**
 * @brief Mount SPIFFS at BSP_SPIFFS_MOUNT_POINT (partition label from menuconfig, default "storage")
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_spiffs_mount(void);

/**
 * @brief Unmount SPIFFS
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_spiffs_unmount(void);

/**************************************************************************************************
 *
 * RGB LED: one WS2812B on GPIO34
 *
 **************************************************************************************************/

/**
 * @brief Create the LED driver (RMT) and switch the LED off
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_led_rgb_init(void);

/**
 * @brief Set the LED color; (0, 0, 0) switches it off
 *
 * Initializes the LED on first use.
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_led_rgb_set(uint8_t red, uint8_t green, uint8_t blue);

/**************************************************************************************************
 *
 * Wi-Fi: ESP32-C5 co-processor over SDIO (ESP-Hosted). Station mode.
 *
 **************************************************************************************************/

/**
 * @brief Initialize NVS, netif and the Wi-Fi driver (STA mode) through ESP-Hosted
 *
 * Needs the SDIO transport and the esp_wifi_remote hosted library selected in sdkconfig.
 *
 * @return ESP_OK on success
 */
esp_err_t bsp_wifi_init(void);

#ifdef __cplusplus
}
#endif
