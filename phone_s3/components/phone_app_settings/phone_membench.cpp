#include "phone_membench.h"
#include <cstring>
#include <mutex>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

const char *TAG = "MEMBENCH";
constexpr size_t PSRAM_BUF = 1024 * 1024;       // 4 x the L2 cache: the PSRAM is measured, not the cache
constexpr size_t SRAM_BUF = 16 * 1024;
constexpr size_t PSRAM_TOTAL = 32 * 1024 * 1024;  // bytes moved per test
constexpr size_t SRAM_TOTAL = 64 * 1024 * 1024;

std::mutex s_mtx;
phone_membench_t s_res;

uint32_t mbps(size_t bytes, int64_t us)
{
    return us > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(bytes) * 1000000 / us / (1024 * 1024)) : 0;
}

uint32_t test_write(uint8_t *buf, size_t size, size_t total)
{
    const int rounds = static_cast<int>(total / size);
    const int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < rounds; r++) {
        memset(buf, r, size);
    }
    return mbps(static_cast<size_t>(rounds) * size, esp_timer_get_time() - t0);
}

uint32_t test_read(const uint8_t *buf, size_t size, size_t total)
{
    const int rounds = static_cast<int>(total / size);
    const auto *p = reinterpret_cast<const volatile uint32_t *>(buf);
    const size_t words = size / 4;
    uint32_t sum = 0;
    const int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < rounds; r++) {
        for (size_t i = 0; i < words; i += 8) {
            sum += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
        }
    }
    const int64_t us = esp_timer_get_time() - t0;
    if (sum == 0x12345678) {
        ESP_LOGD(TAG, "sum");       // keeps the loads
    }
    return mbps(static_cast<size_t>(rounds) * size, us);
}

uint32_t test_copy(uint8_t *dst, const uint8_t *src, size_t size, size_t total)
{
    const int rounds = static_cast<int>(total / size);
    const int64_t t0 = esp_timer_get_time();
    for (int r = 0; r < rounds; r++) {
        memcpy(dst, src, size);
    }
    return mbps(static_cast<size_t>(rounds) * size, esp_timer_get_time() - t0);
}

void task(void *)
{
    phone_membench_t r = {};
    r.psram_mhz = CONFIG_SPIRAM_SPEED;
    auto *pa = static_cast<uint8_t *>(heap_caps_aligned_alloc(128, PSRAM_BUF, MALLOC_CAP_SPIRAM));
    auto *pb = static_cast<uint8_t *>(heap_caps_aligned_alloc(128, PSRAM_BUF, MALLOC_CAP_SPIRAM));
    auto *sa = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, SRAM_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    auto *sb = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, SRAM_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (pa && pb && sa && sb) {
        memset(pa, 1, PSRAM_BUF);
        memset(pb, 2, PSRAM_BUF);
        r.psram_write = test_write(pa, PSRAM_BUF, PSRAM_TOTAL);
        r.psram_read = test_read(pa, PSRAM_BUF, PSRAM_TOTAL);
        r.psram_copy = test_copy(pb, pa, PSRAM_BUF, PSRAM_TOTAL);
        r.sram_write = test_write(sa, SRAM_BUF, SRAM_TOTAL);
        r.sram_read = test_read(sa, SRAM_BUF, SRAM_TOTAL);
        r.sram_copy = test_copy(sb, sa, SRAM_BUF, SRAM_TOTAL);
        r.done = true;
        ESP_LOGI(TAG, "PSRAM %u MHz: write %u, read %u, copy %u MB/s | SRAM: write %u, read %u, copy %u MB/s",
                 static_cast<unsigned>(r.psram_mhz), static_cast<unsigned>(r.psram_write),
                 static_cast<unsigned>(r.psram_read), static_cast<unsigned>(r.psram_copy),
                 static_cast<unsigned>(r.sram_write), static_cast<unsigned>(r.sram_read),
                 static_cast<unsigned>(r.sram_copy));
    } else {
        r.failed = true;
        ESP_LOGW(TAG, "no memory for the buffers");
    }
    heap_caps_free(pa);
    heap_caps_free(pb);
    heap_caps_free(sa);
    heap_caps_free(sb);
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_res = r;
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

extern "C" bool phone_membench_start(void)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_res.running) {
            return false;
        }
        s_res = {};
        s_res.running = true;
    }
    /* on core 1, away from the LVGL task; the stack in PSRAM does not touch the buffers measured */
    if (xTaskCreatePinnedToCoreWithCaps(task, "membench", 4096, nullptr, 3, nullptr, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_res.running = false;
        s_res.failed = true;
        return false;
    }
    return true;
}

extern "C" void phone_membench_get(phone_membench_t *out)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    *out = s_res;
}
