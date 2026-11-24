#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Screen power policy and lock screen.
 *
 *   in use  --idle--> dimmed (3 %) --idle--> lock screen, backlight off, LVGL paused
 *   in use  --power key--> lock screen --power key--> backlight off
 *   off     --power key or charger plugged in--> lock screen
 *   lock screen --slide to unlock--> in use
 *
 * Touching a dimmed screen restores the brightness and the touch still reaches the app.
 */

/** Start the policy; call once with the LVGL lock held, after the phone UI exists. */
void phone_lock_start(void);
/** Power key short press (any task). */
void phone_lock_on_power_key(void);
/** Charger plugged in: light the screen up on the lock screen with the charging effect (any task). */
void phone_lock_on_charger(void);
/** Make sure the screen is on and LVGL runs, e.g. before showing the power menu (any task except LVGL). */
void phone_lock_wake(void);

/** True while the lock screen is shown or the screen is off. */
bool phone_lock_is_locked(void);

/** Screen timeout choices for the settings page; index stored in NVS ("scr_to"). */
int phone_lock_timeout_count(void);
const char *phone_lock_timeout_label(int index);
int phone_lock_timeout_get(void);
void phone_lock_timeout_set(int index);

#ifdef __cplusplus
}
#endif
