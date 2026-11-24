/*
 * AXP2101 battery, charger and power key for the KSDIY ESP32-P4C5 board.
 *
 * Hardware: battery connector J2 with a 10 kOhm NTC on TS, red charge LED on CHGLED, power key
 * BTN1 on PWRON, IRQ (open drain, active low) level-shifted to GPIO21.
 */
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "axp2101.h"

#include "bsp/esp-bsp.h"
#include "bsp_pmic.h"

static const char *TAG = "esp32p4c5_pmic";

#define BSP_PMIC_IRQ_GPIO           (GPIO_NUM_21)
#define BSP_PMIC_TASK_STACK         (3072)
#define BSP_PMIC_TASK_PRIO          (5)
/* Safety net: service the IRQ line even if an edge was missed while it was already low */
#define BSP_PMIC_IRQ_POLL_MS        (1000)
#define BSP_PMIC_POWER_OFF_WAIT_MS  (1000)

/* PMU status 1 (0x00) */
#define PMU_STATUS1_VBUS_GOOD       BIT5
/* Common config (0x10) */
#define PMU_COMMON_SOFT_PWROFF      BIT0
/* IRQ enable/status bits, registers 0x40/0x48, 0x41/0x49, 0x42/0x4A */
#define IRQ0_SOC_LOW                (BIT7 | BIT6)       /* SOC dropped to warning level 1 or 2 */
#define IRQ1_VBUS_IN                BIT7
#define IRQ1_VBUS_OUT               BIT6
#define IRQ1_BATT_IN                BIT5
#define IRQ1_BATT_OUT               BIT4
#define IRQ1_KEY_SHORT              BIT3
#define IRQ1_KEY_LONG               BIT2
#define IRQ2_CHG_DONE               BIT4
#define IRQ2_CHG_START              BIT3

#define BSP_PMIC_READ_RETRIES       (3)
#define BSP_PMIC_READ_RETRY_MS      (5)

static bool s_battery_ready;
static TaskHandle_t s_irq_task;
static bsp_pmic_event_cb_t s_event_cb;
static void *s_event_arg;

/**************************************************************************************************
 * Battery and charger
 **************************************************************************************************/

/* Charge target voltage register 0x64[2:0] */
static uint16_t charge_voltage_mv(uint8_t reg)
{
    static const uint16_t mv[] = {0, 4000, 4100, 4200, 4350, 4400};
    reg &= 0x07;
    return reg < sizeof(mv) / sizeof(mv[0]) ? mv[reg] : 0;
}

#if CONFIG_BSP_PMIC_SET_CHARGER
/* Fast charge current register 0x62[4:0]: 25 mA steps up to 200 mA, then 100 mA steps. */
static uint8_t charge_current_reg(int ma)
{
    return (ma <= 200) ? (uint8_t)(ma / 25) : (uint8_t)(8 + (ma - 200) / 100);
}

static esp_err_t bsp_charger_configure(void)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(axp2101_get_fastcharge_setting(&v), TAG, "read charge current failed");
    v = (v & ~0x1F) | charge_current_reg(CONFIG_BSP_PMIC_CHARGE_CURRENT_MA);
    ESP_RETURN_ON_ERROR(axp2101_set_fastcharge_setting(v), TAG, "set charge current failed");

    ESP_RETURN_ON_ERROR(axp2101_get_chg_voltage_setting(&v), TAG, "read charge voltage failed");
    v = (v & ~0x07) | CONFIG_BSP_PMIC_CHARGE_VOLTAGE_REG;
    ESP_RETURN_ON_ERROR(axp2101_set_chg_voltage_setting(v), TAG, "set charge voltage failed");
    ESP_LOGI(TAG, "Charger set to %d mA, %u mV", CONFIG_BSP_PMIC_CHARGE_CURRENT_MA,
             charge_voltage_mv(CONFIG_BSP_PMIC_CHARGE_VOLTAGE_REG));
    return ESP_OK;
}
#endif

esp_err_t bsp_battery_init(void)
{
    if (s_battery_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "PMIC init failed");
    /* VBAT, VBUS, VSYS and die temperature ADC channels, plus the fuel gauge */
    ESP_RETURN_ON_ERROR(axp2101_enable_pmu_adc_channels(), TAG, "ADC enable failed");
#if CONFIG_BSP_PMIC_SET_CHARGER
    ESP_RETURN_ON_ERROR(bsp_charger_configure(), TAG, "charger config failed");
#endif
    s_battery_ready = true;
    return ESP_OK;
}

/* PMIC transfers are occasionally NACKed: retry a read a few times before giving up. */
#define PMIC_READ(call, what) do {                                                          \
        esp_err_t err_ = ESP_FAIL;                                                          \
        for (int try_ = 0; try_ < BSP_PMIC_READ_RETRIES && err_ != ESP_OK; try_++) {        \
            if (try_) {                                                                     \
                vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_READ_RETRY_MS));                          \
            }                                                                               \
            err_ = (call);                                                                  \
        }                                                                                   \
        ESP_RETURN_ON_ERROR(err_, TAG, "%s read failed: %s", what, esp_err_to_name(err_));  \
    } while (0)

esp_err_t bsp_battery_get_info(bsp_battery_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "info is NULL");
    ESP_RETURN_ON_ERROR(bsp_battery_init(), TAG, "battery init failed");

    *info = (bsp_battery_info_t) {
        .percent = -1,
    };
    axp2101_batt_status_t st;
    PMIC_READ(axp2101_get_battery_status(&st), "status");
    switch (st) {
    case AXP2101_BATT_CHARGING:
        info->state = BSP_BATTERY_CHARGING;
        break;
    case AXP2101_BATT_CHARGED:
        info->state = BSP_BATTERY_FULL;
        break;
    case AXP2101_BATT_DISCHARGING:
        info->state = BSP_BATTERY_DISCHARGING;
        break;
    default:
        info->state = BSP_BATTERY_ABSENT;
        break;
    }

    uint8_t st1 = 0;
    PMIC_READ(axp2101_get_pmu_status_1(&st1), "status");
    info->vbus_present = (st1 & PMU_STATUS1_VBUS_GOOD) != 0;

    if (info->state != BSP_BATTERY_ABSENT) {
        uint8_t pct = 0;
        PMIC_READ(axp2101_get_battery_percentage(&pct), "gauge");
        info->percent = pct > 100 ? 100 : pct;
        PMIC_READ(axp2101_get_vbat_voltage_mv(&info->voltage_mv), "VBAT");
    }
    if (info->vbus_present) {
        PMIC_READ(axp2101_get_vbus_voltage_mv(&info->vbus_mv), "VBUS");
    }
    PMIC_READ(axp2101_get_vsys_voltage_mv(&info->vsys_mv), "VSYS");
    PMIC_READ(axp2101_get_die_temperature_c(&info->die_temp_c), "TDIE");

    uint8_t reg = 0;
    PMIC_READ(axp2101_get_fastcharge_setting(&reg), "charger");
    info->charge_current_ma = axp2101_decode_fastcharge_ma(reg);
    PMIC_READ(axp2101_get_chg_voltage_setting(&reg), "charger");
    info->charge_voltage_mv = charge_voltage_mv(reg);
    return ESP_OK;
}

/**************************************************************************************************
 * Events (power key, VBUS, battery, charger) from the AXP2101 IRQ line
 **************************************************************************************************/

static void IRAM_ATTR bsp_pmic_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_irq_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void bsp_pmic_dispatch(bsp_pmic_event_t event)
{
    if (s_event_cb) {
        s_event_cb(event, s_event_arg);
    }
}

/* Read and clear (write 1) the three IRQ status registers, then report the events. */
static void bsp_pmic_service_irq(void)
{
    uint8_t s0 = 0, s1 = 0, s2 = 0;
    if (axp2101_get_irq_status_0(&s0) != ESP_OK || axp2101_get_irq_status_1(&s1) != ESP_OK ||
            axp2101_get_irq_status_2(&s2) != ESP_OK) {
        ESP_LOGW(TAG, "IRQ status read failed");
        return;
    }
    (void)axp2101_set_irq_status_0(s0);
    (void)axp2101_set_irq_status_1(s1);
    (void)axp2101_set_irq_status_2(s2);

    if (s1 & IRQ1_KEY_SHORT) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_KEY_SHORT_PRESS);
    }
    if (s1 & IRQ1_KEY_LONG) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_KEY_LONG_PRESS);
    }
    if (s1 & IRQ1_VBUS_IN) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_VBUS_INSERTED);
    }
    if (s1 & IRQ1_VBUS_OUT) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_VBUS_REMOVED);
    }
    if (s1 & IRQ1_BATT_IN) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_BATTERY_INSERTED);
    }
    if (s1 & IRQ1_BATT_OUT) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_BATTERY_REMOVED);
    }
    if (s2 & IRQ2_CHG_START) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_CHARGE_START);
    }
    if (s2 & IRQ2_CHG_DONE) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_CHARGE_DONE);
    }
    if (s0 & IRQ0_SOC_LOW) {
        bsp_pmic_dispatch(BSP_PMIC_EVENT_BATTERY_LOW);
    }
}

static void bsp_pmic_irq_task(void *arg)
{
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BSP_PMIC_IRQ_POLL_MS));
        while (gpio_get_level(BSP_PMIC_IRQ_GPIO) == 0) {
            bsp_pmic_service_irq();
        }
    }
}

esp_err_t bsp_pmic_event_register(bsp_pmic_event_cb_t cb, void *arg)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "callback is NULL");
    ESP_RETURN_ON_FALSE(s_irq_task == NULL, ESP_ERR_INVALID_STATE, TAG, "callback already registered");
    ESP_RETURN_ON_ERROR(bsp_battery_init(), TAG, "battery init failed");
    s_event_cb = cb;
    s_event_arg = arg;

    /* Only the sources reported above may pull the line low; drop anything already pending */
    ESP_RETURN_ON_ERROR(axp2101_set_irq_enable_0(IRQ0_SOC_LOW), TAG, "IRQ enable failed");
    ESP_RETURN_ON_ERROR(axp2101_set_irq_enable_1(IRQ1_VBUS_IN | IRQ1_VBUS_OUT | IRQ1_BATT_IN | IRQ1_BATT_OUT |
                                                 IRQ1_KEY_SHORT | IRQ1_KEY_LONG), TAG, "IRQ enable failed");
    ESP_RETURN_ON_ERROR(axp2101_set_irq_enable_2(IRQ2_CHG_DONE | IRQ2_CHG_START), TAG, "IRQ enable failed");
    ESP_RETURN_ON_ERROR(axp2101_set_irq_status_0(0xFF), TAG, "IRQ clear failed");
    ESP_RETURN_ON_ERROR(axp2101_set_irq_status_1(0xFF), TAG, "IRQ clear failed");
    ESP_RETURN_ON_ERROR(axp2101_set_irq_status_2(0xFF), TAG, "IRQ clear failed");

    if (xTaskCreate(bsp_pmic_irq_task, "bsp_pmic", BSP_PMIC_TASK_STACK, NULL, BSP_PMIC_TASK_PRIO,
                    &s_irq_task) != pdPASS) {
        s_event_cb = NULL;
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t io = {
        .pin_bit_mask = BIT64(BSP_PMIC_IRQ_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "IRQ GPIO config failed");
    /* The touch driver may have installed the GPIO ISR service already */
    esp_err_t err = gpio_isr_handler_add(BSP_PMIC_IRQ_GPIO, bsp_pmic_isr, NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "ISR service failed");
        err = gpio_isr_handler_add(BSP_PMIC_IRQ_GPIO, bsp_pmic_isr, NULL);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "ISR add failed");
    return ESP_OK;
}

/**************************************************************************************************
 * Power off
 **************************************************************************************************/

esp_err_t bsp_power_off(void)
{
    ESP_RETURN_ON_ERROR(bsp_pmic_init(), TAG, "PMIC init failed");
    /* Blank the panel first; ignored when the display was never started */
    (void)bsp_display_brightness_set(0);

    uint8_t cfg = 0;
    ESP_RETURN_ON_ERROR(axp2101_get_pmu_common_cfg(&cfg), TAG, "read failed");
    ESP_LOGW(TAG, "Power off");
    ESP_RETURN_ON_ERROR(axp2101_set_pmu_common_cfg(cfg | PMU_COMMON_SOFT_PWROFF), TAG, "power off failed");

    /* The rails drop within a few ms; still running here means the PMIC kept them up */
    vTaskDelay(pdMS_TO_TICKS(BSP_PMIC_POWER_OFF_WAIT_MS));
    return ESP_FAIL;
}
