#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * System monitor: a small box over every screen with the CPU load of both cores, the frames drawn
 * per second, the internal SRAM (free, lowest, largest block), the DMA-capable internal RAM and the
 * PSRAM. The same figures go to the log every 5 s. Off by default; Settings > Debug turns it on
 * (NVS key "mon"; log tag SYSMON). Call with the LVGL lock held.
 */
void phone_monitor_init(void);
void phone_monitor_set(bool on);
bool phone_monitor_is_on(void);

/**
 * Internal RAM report in the log (tag MEMDUMP): task stacks, used heap blocks by size, the largest
 * ones and the blocks taken since the last report. Made each time the monitor is turned on.
 */
void phone_memdump_report(void);

#ifdef __cplusplus
}
#endif
