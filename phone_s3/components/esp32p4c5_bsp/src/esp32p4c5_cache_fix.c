/*
 * ESP32-P4 (rev >= v3.0) L1 D-cache arbitration workaround.
 *
 * ESP-IDF v6.1 cache_hal_init() sets the cache "undef_op" preload strategy of
 * every cache to CACHE_LL_PRELOAD_ARBITRARY (2, random arbitration). On the
 * ESP32-P4 this can livelock the L1 D-cache when one core spins on a cached
 * address while the other core misses. At boot CPU1 spins this way while it
 * waits for the scheduler, and CPU0 then faults (for example "Store access
 * fault" in the ROM memset called from prvInitialiseNewTask), depending on the
 * memory layout.
 *
 * Espressif's advice (https://github.com/espressif/esp-idf/issues/19369) is to
 * set only the L1 D-cache back to round-robin (0); the L1 I-cache and the L2
 * cache can stay at 2. This constructor does that before any other global
 * constructor and before app_main().
 *
 * The component CMakeLists.txt passes "-u esp32p4c5_bsp_l1_dcache_round_robin"
 * to the linker, so the constructor is kept even when nothing references this
 * file.
 */

#include "sdkconfig.h"

void esp32p4c5_bsp_l1_dcache_round_robin(void);

#if CONFIG_IDF_TARGET_ESP32P4 && __has_include("hal/config.h")
#include "hal/config.h"
#include "hal/cache_ll.h"

#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) >= 300
__attribute__((constructor(101))) void esp32p4c5_bsp_l1_dcache_round_robin(void)
{
    cache_ll_preload_set_strategy(1, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_UNTIL_FETCH_DONE);
}
#define ESP32P4C5_CACHE_FIX_DEFINED 1
#endif
#endif

#ifndef ESP32P4C5_CACHE_FIX_DEFINED
/* Not needed on this target or chip revision; keep the symbol for the -u flag. */
void esp32p4c5_bsp_l1_dcache_round_robin(void)
{
}
#endif
