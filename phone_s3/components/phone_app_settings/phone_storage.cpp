#include "phone_storage.h"
#include <atomic>
#include <cstring>
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"

#define TAG "SD"

namespace {

std::atomic<int> s_state{PHONE_STORAGE_DISABLED};
constexpr int MAX_HOLDERS = 4;
const char *s_holders[MAX_HOLDERS];
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* The card shares the SDMMC host with the SDIO link of the C5 (slot 1): the automatic CMD12 that ends a
 * multiple block write sometimes reaches it broken and leaves it in the receive state. Writes go as
 * CMD23 + CMD25 instead (local esp_driver_sdmmc, README_LOCAL.md). Set before each mount. */
esp_err_t mount_card()
{
    sdmmc_host_set_counted_writes(SDMMC_HOST_SLOT_0, true);
    return bsp_sdcard_mount();
}

void mount_task(void *)
{
    /* The BSP never formats the card when mounting fails */
    esp_err_t err = mount_card();
    s_state = err == ESP_OK ? PHONE_STORAGE_MOUNTED : PHONE_STORAGE_NO_CARD;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No microSD card mounted: %s", esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void phone_storage_start(void)
{
#if CONFIG_PHONE_SD_CARD_MOUNT
    if (s_state != PHONE_STORAGE_DISABLED) {
        return;
    }
    s_state = PHONE_STORAGE_CHECKING;
    if (xTaskCreate(mount_task, "sd_mount", 4096, nullptr, 3, nullptr) != pdPASS) {
        s_state = PHONE_STORAGE_NO_CARD;
    }
#endif
}

extern "C" phone_storage_state_t phone_storage_get(uint64_t *total, uint64_t *free)
{
    auto state = static_cast<phone_storage_state_t>(s_state.load());
    if (state == PHONE_STORAGE_MOUNTED) {
        uint64_t t = 0, f = 0;
        if (esp_vfs_fat_info(CONFIG_BSP_SD_MOUNT_POINT, &t, &f) != ESP_OK) {
            t = f = 0;
        }
        if (total) {
            *total = t;
        }
        if (free) {
            *free = f;
        }
    }
    return state;
}

extern "C" void phone_storage_hold(const char *who)
{
    portENTER_CRITICAL(&s_lock);
    for (auto &h : s_holders) {
        if (h == who) {
            break;
        }
        if (!h) {
            h = who;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

extern "C" void phone_storage_release(const char *who)
{
    portENTER_CRITICAL(&s_lock);
    for (auto &h : s_holders) {
        if (h == who) {
            h = nullptr;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

extern "C" int phone_storage_holders(char *buf, size_t len)
{
    const char *names[MAX_HOLDERS];
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    for (const char *h : s_holders) {
        if (h) {
            names[n++] = h;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (buf && len) {
        buf[0] = '\0';
        for (int i = 0; i < n; i++) {
            if (i) {
                strlcat(buf, ", ", len);
            }
            strlcat(buf, names[i], len);
        }
    }
    return n;
}

extern "C" esp_err_t phone_storage_lend(sdmmc_card_t **card)
{
    if (s_state != PHONE_STORAGE_MOUNTED || phone_storage_holders(nullptr, 0) > 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = bsp_sdcard_unmount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "unmount: %s", esp_err_to_name(err));
        return err;
    }
    s_state = PHONE_STORAGE_USB;
    err = bsp_sdcard_raw_init(card);
    if (err != ESP_OK) {
        phone_storage_take_back();
    }
    return err;
}

extern "C" esp_err_t phone_storage_recover(void)
{
    if (s_state != PHONE_STORAGE_MOUNTED) {
        return ESP_ERR_INVALID_STATE;
    }
    char holders[64];
    if (phone_storage_holders(holders, sizeof(holders)) > 0) {
        ESP_LOGW(TAG, "card stuck, not remounted: held by %s", holders);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "card stuck: power cycle and remount");
    s_state = PHONE_STORAGE_CHECKING;
    /* unmount releases slot 0 only (slot 1 is the SDIO link of the C5) and the LDO of the card */
    esp_err_t err = bsp_sdcard_unmount();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));     // let the card supply fall
        err = mount_card();
    }
    s_state = err == ESP_OK ? PHONE_STORAGE_MOUNTED : PHONE_STORAGE_NO_CARD;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "recover: %s", esp_err_to_name(err));
    }
    return err;
}

extern "C" esp_err_t phone_storage_take_back(void)
{
    if (s_state != PHONE_STORAGE_USB) {
        return ESP_OK;
    }
    bsp_sdcard_raw_deinit();
    /* The BSP never formats the card when mounting fails */
    esp_err_t err = mount_card();
    s_state = err == ESP_OK ? PHONE_STORAGE_MOUNTED : PHONE_STORAGE_NO_CARD;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "remount: %s", esp_err_to_name(err));
    }
    return err;
}
