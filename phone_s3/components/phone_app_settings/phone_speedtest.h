#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Network speed test (Settings > Debug > Network speed): an HTTP download or upload for 10 s, in its
 * own task, over the default network interface (Wi-Fi now; the 4G modem can use it too).
 * Targets: a PC on the same network running tools/speed_server.py (found by its UDP broadcast
 * on port 8081, else the Wi-Fi gateway on port 8080) or speed.cloudflare.com (plain HTTP: no TLS cost in the figure). Results also go to the log (tag SPEEDTEST).
 */
typedef enum {
    PHONE_SPEED_LAN,
    PHONE_SPEED_NET,
} phone_speed_target_t;

typedef struct {
    bool running;
    bool upload;
    bool failed;
    uint64_t bytes;
    uint32_t ms;
    uint32_t kbps;            /**< so far, then the result */
    uint32_t sram_min;        /**< lowest free internal RAM during the test, bytes */
    uint32_t dma_min;         /**< lowest free DMA-capable internal RAM during the test, bytes */
    char host[64];
} phone_speed_state_t;

/** Start a test; false if one is running or the task cannot start */
bool phone_speedtest_start(phone_speed_target_t target, bool upload);
void phone_speedtest_get(phone_speed_state_t *out);

#ifdef __cplusplus
}
#endif
