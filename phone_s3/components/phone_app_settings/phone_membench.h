#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Memory speed test (Settings > Debug > Memory speed): write (memset), read (32-bit loads) and copy
 * (memcpy) on PSRAM (1 MB buffers, larger than the 256 KB L2 cache, so the PSRAM itself is measured)
 * and on internal SRAM (16 KB buffers), in MB/s. Runs in its own task; results also go to the log
 * (tag MEMBENCH).
 */
typedef struct {
    bool running;
    bool done;
    bool failed;
    uint32_t psram_write, psram_read, psram_copy;   /**< MB/s */
    uint32_t sram_write, sram_read, sram_copy;
    uint32_t psram_mhz;
} phone_membench_t;

bool phone_membench_start(void);
void phone_membench_get(phone_membench_t *out);

#ifdef __cplusplus
}
#endif
