#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Volume keys: IO0 (GPIO0) = volume up, BOOT (GPIO35) = volume down, 5 % per press, repeating
 * while held. A small vertical volume panel shows under the status bar. The keys do nothing on the
 * lock screen or with the screen off. Volume 0 % is "muted", shared with the Settings > Sound page;
 * volume up while muted goes back to the volume set before muting.
 */
void phone_volume_start(void);

#ifdef __cplusplus
}
#endif
