#include "phone_paths.h"
#include "phone_sdbench.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "phone_storage.h"
#include "sdkconfig.h"

namespace {

const char *TAG = "SDBENCH";
const char *HOLDER = "SD test";
constexpr size_t FILE_SIZE = 8 * 1024 * 1024;
constexpr size_t SRAM_BUF = 32 * 1024;
constexpr size_t PSRAM_BUF = 64 * 1024;
const char *PATH = PHONE_SD_SDBENCH;

std::mutex s_mtx;
phone_sdbench_t s_res;

uint32_t kbps(size_t bytes, int64_t us)
{
    return us > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(bytes) * 1000000 / us / 1024) : 0;
}

/* Write then read the file with one buffer; false on an I/O error */
bool run(uint8_t *buf, size_t size, uint32_t *wr, uint32_t *rd)
{
    for (size_t i = 0; i < size; i++) {
        buf[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    phone_sd_make_dirs(PATH);
    FILE *f = fopen(PATH, "wb");
    if (!f) {
        return false;
    }
    setvbuf(f, nullptr, _IONBF, 0);         /* straight to FAT: the buffer given is the one measured */
    int64_t t0 = esp_timer_get_time();
    size_t done = 0;
    while (done < FILE_SIZE && fwrite(buf, 1, size, f) == size) {
        done += size;
    }
    fsync(fileno(f));
    const bool closed = fclose(f) == 0;
    *wr = kbps(done, esp_timer_get_time() - t0);
    if (done < FILE_SIZE || !closed) {
        return false;
    }
    f = fopen(PATH, "rb");
    if (!f) {
        return false;
    }
    setvbuf(f, nullptr, _IONBF, 0);
    t0 = esp_timer_get_time();
    done = 0;
    size_t n;
    while (done < FILE_SIZE && (n = fread(buf, 1, size, f)) > 0) {
        done += n;
    }
    fclose(f);
    *rd = kbps(done, esp_timer_get_time() - t0);
    return done == FILE_SIZE;
}

void task(void *)
{
    phone_sdbench_t r = {};
    uint64_t free_bytes = 0;
    phone_storage_hold(HOLDER);
    auto *sb = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, SRAM_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    auto *pb = static_cast<uint8_t *>(heap_caps_aligned_alloc(128, PSRAM_BUF, MALLOC_CAP_SPIRAM));
    if (phone_storage_get(nullptr, &free_bytes) != PHONE_STORAGE_MOUNTED) {
        snprintf(r.error, sizeof(r.error), "no card");
    } else if (free_bytes < FILE_SIZE + 1024 * 1024) {
        snprintf(r.error, sizeof(r.error), "card full");
    } else if (!sb || !pb) {
        snprintf(r.error, sizeof(r.error), "no memory");
    } else if (!run(sb, SRAM_BUF, &r.sram_write, &r.sram_read) ||
               !run(pb, PSRAM_BUF, &r.psram_write, &r.psram_read)) {
        snprintf(r.error, sizeof(r.error), "I/O error");
    } else {
        r.done = true;
    }
    unlink(PATH);
    heap_caps_free(sb);
    heap_caps_free(pb);
    phone_storage_release(HOLDER);
    r.failed = !r.done;
    if (r.done) {
        ESP_LOGI(TAG, "8 MB file: SRAM buffer write %u, read %u KB/s | PSRAM buffer write %u, read %u KB/s",
                 static_cast<unsigned>(r.sram_write), static_cast<unsigned>(r.sram_read),
                 static_cast<unsigned>(r.psram_write), static_cast<unsigned>(r.psram_read));
    } else {
        ESP_LOGW(TAG, "failed: %s", r.error);
    }
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_res = r;
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

extern "C" bool phone_sdbench_start(void)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_res.running) {
            return false;
        }
        s_res = {};
        s_res.running = true;
    }
    if (xTaskCreatePinnedToCoreWithCaps(task, "sdbench", 4096, nullptr, 3, nullptr, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_res.running = false;
        s_res.failed = true;
        snprintf(s_res.error, sizeof(s_res.error), "no task");
        return false;
    }
    return true;
}

extern "C" void phone_sdbench_get(phone_sdbench_t *out)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    *out = s_res;
}
