#pragma once

#include <stdbool.h>
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHONE_POWER_MENU = 0,          /*!< Power off / Restart / Cancel (power key long press) */
    PHONE_POWER_OFF,               /*!< "Power off?" */
    PHONE_POWER_RESTART,           /*!< "Restart?" */
    PHONE_POWER_RESTART_LANG,      /*!< "Restart to apply language?" */
} phone_power_action_t;

/**
 * Start the PMIC service: battery polling (every 5 s and on USB/charger events) and the power key
 * (short press: lock screen then screen off, long press: power menu).
 */
void phone_power_start(void);
/** Latest battery reading; false until the first one succeeded. */
bool phone_power_get_battery(bsp_battery_info_t *out);
/** Show the power dialog on the top layer; call from LVGL context. */
void phone_power_confirm(phone_power_action_t action);

#ifdef __cplusplus
}
#endif
