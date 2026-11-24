/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "phone_common_net.hpp"
#include "phone_net.h"
#include "phone_locale.h"
#include "phone_fonts.h"
#include "phone_audio.h"
#include "phone_power.h"
#include "phone_storage.h"
#include "phone_lock.h"
#include "phone_volume.h"
#include "phone_monitor.h"
#include "phone_quick.h"
#include "phone_boot_splash.h"
#include "nvs_flash.h"
#include "boost/thread.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"

using namespace esp_brookesia;

constexpr int WIFI_ICON_SEARCHING = 4;  // added to the stock Wi-Fi icon: off 0, levels 1..3, searching 4
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

constexpr bool EXAMPLE_SHOW_MEM_INFO = false;

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("Display ESP-Brookesia phone demo");

    /* NVS holds the settings app preferences (language, time zone, brightness, saved Wi-Fi) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    phone_locale_init();
    phone_audio_set_volume(phone_pref_get("mute", 0) ? 0 : phone_pref_get("vol", 60));

    phone_common_net_boot();

    /* Start display (LVGL adapter + panel + touch) */
    ESP_UTILS_CHECK_NULL_EXIT(bsp_display_start(), "Start display failed");
    /* Animated logo while the phone starts */
    if (bsp_display_lock(0)) {
        phone_boot_splash_show();
        bsp_display_unlock();
        /* the LVGL task draws the first frame before the backlight comes on, instead of the blank
         * frame buffer (lv_refr_now() here overflows the main task stack) */
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    ESP_UTILS_CHECK_ERROR_EXIT(bsp_display_brightness_set(phone_pref_get("bri", 100)), "Turn on display backlight failed");

    /* Configure GUI lock */
    LvLock::registerCallbacks([](int timeout_ms) {
        if (timeout_ms < 0) {
            timeout_ms = 0;
        } else if (timeout_ms == 0) {
            timeout_ms = 1;
        }
        ESP_UTILS_CHECK_FALSE_RETURN(bsp_display_lock(timeout_ms), false, "Lock failed");

        return true;
    }, []() {
        bsp_display_unlock();

        return true;
    });

    /* Create a phone object */
    Phone *phone = new (std::nothrow) Phone();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");

    /* Try using a stylesheet that corresponds to the resolution */
    Stylesheet *stylesheet = nullptr;
    if ((BSP_LCD_H_RES == 480) && (BSP_LCD_V_RES == 800)) {
        stylesheet = new (std::nothrow) Stylesheet(STYLESHEET_480_800_DARK);
        ESP_UTILS_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    }
    if (stylesheet) {
        /* The stock system fonts have no Vietnamese glyphs (app names, recents titles): swap the
         * sizes covered by the settings app fonts, which are the same typeface */
        auto &text = stylesheet->core.display.text;
        for (int i = 0; i < text.default_fonts_num; i++) {
            if (text.default_fonts[i].size_px >= 12 && text.default_fonts[i].size_px <= 28) {
                text.default_fonts[i].font_resource = phone_font_by_size(text.default_fonts[i].size_px);
            }
        }
        /* Wi-Fi icon: a 5th image, the full fan in the grey of the unlit 4G bars (white at 0x50 over the 0x38393A bar), for "on but not connected"
         * (connecting, or no saved network in range); "off" keeps the stock crossed icon */
        auto &wifi_icon = stylesheet->display.status_bar.data.wifi.icon_data.icon;
        if (wifi_icon.image_num == 4) {
            wifi_icon.images[WIFI_ICON_SEARCHING] = gui::StyleImage::IMAGE_RECOLOR(wifi_icon.images[3].resource, 0x767778);
            wifi_icon.image_num = 5;
        }
        ESP_UTILS_LOGI("Using stylesheet (%s)", stylesheet->core.name);
        ESP_UTILS_CHECK_FALSE_EXIT(phone->addStylesheet(stylesheet), "Add stylesheet failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->activateStylesheet(stylesheet), "Activate stylesheet failed");
        delete stylesheet;
    }

    {
        // When operating on non-GUI tasks, should acquire a lock before operating on LVGL
        LvLockGuard gui_guard;

        /* Begin the phone */
        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");
        /* Brookesia has put its own system layer in place: the splash moves onto it */
        phone_boot_splash_raise();
        // assert(phone->getDisplay().showContainerBorder() && "Show container border failed");

        /* Init and install apps from registry */
        std::vector<systems::base::Manager::RegistryAppInfo> inited_apps;
        ESP_UTILS_CHECK_FALSE_EXIT(phone->initAppFromRegistry(inited_apps), "Init app registry failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installAppFromRegistry(inited_apps), "Install app registry failed");

        /* Wi-Fi service: brings the ESP32-C5 up in the background and reconnects to saved networks */
        phone_net_start();
        /* Battery polling and power key (AXP2101) */
        phone_power_start();
        /* microSD card in the background (never formatted) */
        phone_storage_start();
        /* Dim / lock screen / screen off on inactivity and on the power key */
        phone_lock_start();
        /* BOOT / IO0 as volume up / down */
        phone_volume_start();
        /* CPU / RAM monitor over every screen when turned on in Settings > Debug */
        phone_monitor_init();
        /* Quick settings: pull down from the top edge */
        phone_quick_start(phone);
        /* Everything is up: the splash fades out after its animation */
        phone_boot_splash_ready();

        /* Create a timer to update the clock and the Wi-Fi icon */
        lv_timer_create([](lv_timer_t *t) {
            static int s_wifi = -1;
            time_t now;
            struct tm timeinfo;
            Phone *phone = (Phone *)t->user_data;

            ESP_UTILS_CHECK_NULL_EXIT(phone, "Invalid phone");

            time(&now);
            localtime_r(&now, &timeinfo);

            ESP_UTILS_CHECK_FALSE_EXIT(
                phone->getDisplay().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min),
                "Refresh status bar failed"
            );

            phone_net_status_t st = {};
            phone_net_get_status(&st);
            int wifi = 0;
            if (st.wifi_connected) {
                wifi = st.rssi >= -60 ? 3 : (st.rssi >= -72 ? 2 : 1);
            } else if (phone_net_is_enabled()) {
                wifi = WIFI_ICON_SEARCHING;
            }
            if (wifi != s_wifi) {
                s_wifi = wifi;
                phone->getDisplay().getStatusBar()->setWifiIconState(wifi);
            }

            static int s_bat = -2;
            static bool s_chg;
            bsp_battery_info_t b;
            if (phone_power_get_battery(&b)) {
                auto *bar = phone->getDisplay().getStatusBar();
                int pct = b.state == BSP_BATTERY_ABSENT ? -1 : b.percent;
                bool chg = b.state == BSP_BATTERY_CHARGING;
                if (pct != s_bat || chg != s_chg) {
                    if (pct < 0) {
                        bar->hideBatteryIcon();
                        bar->hideBatteryPercent();
                    } else {
                        if (s_bat < 0) {
                            bar->showBatteryIcon();
                            bar->showBatteryPercent();
                        }
                        bar->setBatteryPercent(chg, pct);
                    }
                    s_bat = pct;
                    s_chg = chg;
                }
            }
        }, 1000, phone);
    }

    if constexpr (EXAMPLE_SHOW_MEM_INFO) {
        esp_utils::thread_config_guard thread_config({
            .name = "mem_info",
            .stack_size = 4096,
        });
        boost::thread([ = ]() {
            char buffer[128];    /* Make sure buffer is enough for `sprintf` */
            size_t internal_free = 0;
            size_t internal_total = 0;
            size_t external_free = 0;
            size_t external_total = 0;

            while (1) {
                internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                sprintf(buffer,
                        "\t           Biggest /     Free /    Total\n"
                        "\t  SRAM : [%8d / %8d / %8d]\n"
                        "\t PSRAM : [%8d / %8d / %8d]",
                        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), internal_free, internal_total,
                        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), external_free, external_total);
                ESP_UTILS_LOGI("\n%s", buffer);

                {
                    LvLockGuard gui_guard;
                    ESP_UTILS_CHECK_FALSE_EXIT(
                        phone->getDisplay().getRecentsScreen()->setMemoryLabel(
                            internal_free / 1024, internal_total / 1024, external_free / 1024, external_total / 1024
                        ), "Set memory label failed"
                    );
                }

                boost::this_thread::sleep_for(boost::chrono::seconds(5));
            }
        }).detach();
    }
}
