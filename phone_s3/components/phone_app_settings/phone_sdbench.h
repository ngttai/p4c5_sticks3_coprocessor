#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * microSD speed test (Settings > Debug > Memory speed): writes then reads an 8 MB file on /sdcard
 * through FAT, once with a 32 KB buffer in internal DMA RAM and once with a 64 KB buffer in PSRAM,
 * in KB/s; the file is deleted afterwards. Holds the card meanwhile (no USB drive). Runs in its own
 * task; results also go to the log (tag SDBENCH).
 */
typedef struct {
    bool running;
    bool done;
    bool failed;
    char error[48];
    uint32_t sram_write, sram_read;     /**< KB/s, internal DMA buffer */
    uint32_t psram_write, psram_read;   /**< KB/s, PSRAM buffer */
} phone_sdbench_t;

bool phone_sdbench_start(void);
void phone_sdbench_get(phone_sdbench_t *out);

#ifdef __cplusplus
}
#endif
