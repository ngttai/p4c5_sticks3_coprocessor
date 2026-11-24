/*
 * SPIFFS on the "storage" partition, mounted at CONFIG_BSP_SPIFFS_MOUNT_POINT.
 * Ported from the esp-bsp ESP32-P4 Function EV Board BSP.
 */
#include "esp_log.h"
#include "esp_spiffs.h"
#include "bsp/esp32p4c5_bsp.h"
#include "bsp_err_check.h"

static const char *TAG = "esp32p4c5_spiffs";
static bool s_spiffs_mounted;

esp_err_t bsp_spiffs_mount(void)
{
    if (s_spiffs_mounted) {
        return ESP_OK;
    }
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info(CONFIG_BSP_SPIFFS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at %s: %u KB used of %u KB", CONFIG_BSP_SPIFFS_MOUNT_POINT,
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    s_spiffs_mounted = true;
    return ESP_OK;
}

esp_err_t bsp_spiffs_unmount(void)
{
    if (!s_spiffs_mounted) {
        return ESP_OK;
    }
    BSP_ERROR_CHECK_RETURN_ERR(esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL));
    s_spiffs_mounted = false;
    return ESP_OK;
}
