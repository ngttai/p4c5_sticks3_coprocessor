#include "assistant_mcp.h"

#include <cstdio>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "phone_audio.h"
#include "phone_locale.h"
#include "phone_net.h"
#include "phone_power.h"

#define TAG "assistant_mcp"

namespace {

constexpr int DEFAULT_BRIGHTNESS = 100;

int s_led[3];     // last color set from here, for the status report

esp_mcp_value_t get_device_status(const esp_mcp_property_list_t *)
{
    char buf[448];
    bsp_battery_info_t bat = {};
    const bool have_bat = phone_power_get_battery(&bat);
    const char *state = "unknown";
    if (have_bat) {
        switch (bat.state) {
        case BSP_BATTERY_ABSENT: state = "absent"; break;
        case BSP_BATTERY_DISCHARGING: state = "discharging"; break;
        case BSP_BATTERY_CHARGING: state = "charging"; break;
        case BSP_BATTERY_FULL: state = "full"; break;
        }
    }
    phone_net_status_t net = {};
    phone_net_get_status(&net);
    const bool online = net.wifi_connected;
    const char *net_type = "wifi";
    const int dbm = net.wifi_connected ? net.rssi : 0;

    int n = snprintf(buf, sizeof(buf),
                     "{\"audio_speaker\":{\"volume\":%d},\"screen\":{\"brightness\":%d},"
                     "\"battery\":{\"percent\":%d,\"state\":\"%s\",\"usb_power\":%s},"
                     "\"network\":{\"type\":\"%s\",\"connected\":%s,\"signal_dbm\":%d},"
                     "\"led\":{\"red\":%d,\"green\":%d,\"blue\":%d}}",
                     phone_audio_get_volume(), phone_pref_get("bri", DEFAULT_BRIGHTNESS),
                     have_bat ? bat.percent : -1, state, (have_bat && bat.vbus_present) ? "true" : "false",
                     net_type, online ? "true" : "false", dbm,
                     s_led[0], s_led[1], s_led[2]);
    if (n < 0 || n >= static_cast<int>(sizeof(buf))) {
        return esp_mcp_value_create_string("{}");
    }
    return esp_mcp_value_create_string(buf);
}

esp_mcp_value_t set_volume(const esp_mcp_property_list_t *props)
{
    const int v = esp_mcp_property_list_get_property_int(props, "volume");
    if (v < 0 || v > 100) {
        return esp_mcp_value_create_bool(false);
    }
    phone_audio_set_volume(v);
    if (v == 0) {
        phone_pref_set("mute", 1);      /* same convention as the volume keys: keep "vol" for unmuting */
    } else {
        phone_pref_set("vol", v);
        phone_pref_set("mute", 0);
    }
    ESP_LOGI(TAG, "volume %d", v);
    return esp_mcp_value_create_bool(true);
}

esp_mcp_value_t set_brightness(const esp_mcp_property_list_t *props)
{
    const int v = esp_mcp_property_list_get_property_int(props, "brightness");
    if (v < 1 || v > 100) {
        return esp_mcp_value_create_bool(false);
    }
    bsp_display_brightness_set(v);
    phone_pref_set("bri", v);
    ESP_LOGI(TAG, "brightness %d", v);
    return esp_mcp_value_create_bool(true);
}

esp_mcp_value_t set_led_color(const esp_mcp_property_list_t *props)
{
    const int r = esp_mcp_property_list_get_property_int(props, "red");
    const int g = esp_mcp_property_list_get_property_int(props, "green");
    const int b = esp_mcp_property_list_get_property_int(props, "blue");
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        return esp_mcp_value_create_bool(false);
    }
    const bool ok = bsp_led_rgb_set(r, g, b) == ESP_OK;
    if (ok) {
        s_led[0] = r;
        s_led[1] = g;
        s_led[2] = b;
    }
    ESP_LOGI(TAG, "led %d %d %d: %s", r, g, b, ok ? "ok" : "failed");
    return esp_mcp_value_create_bool(ok);
}

struct IntProp {
    const char *name;
    int min;
    int max;
};

/** Create a tool, give it integer properties (n may be 0) and add it to the engine. */
bool add_tool(esp_mcp_t *mcp, const char *name, const char *description, esp_mcp_tool_callback_t cb,
              const IntProp *props = nullptr, int n = 0)
{
    esp_mcp_tool_t *tool = esp_mcp_tool_create(name, description, cb);
    if (!tool) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        esp_mcp_property_t *p = esp_mcp_property_create_with_range(props[i].name, props[i].min, props[i].max);
        if (!p || esp_mcp_tool_add_property(tool, p) != ESP_OK) {
            ESP_LOGE(TAG, "property %s.%s failed", name, props[i].name);
            if (p) {
                esp_mcp_property_destroy(p);
            }
            return false;
        }
    }
    if (esp_mcp_add_tool(mcp, tool) != ESP_OK) {
        ESP_LOGE(TAG, "add %s failed", name);
        return false;
    }
    return true;
}

} // namespace

extern "C" bool assistant_mcp_register(esp_mcp_t *mcp)
{
    static const IntProp volume[] = {{"volume", 0, 100}};
    static const IntProp brightness[] = {{"brightness", 1, 100}};
    static const IntProp color[] = {{"red", 0, 255}, {"green", 0, 255}, {"blue", 0, 255}};

    int added = 0;
    added += add_tool(mcp, "self.get_device_status",
                      "Real-time device status: speaker volume, screen brightness, battery level and charging "
                      "state, network (Wi-Fi), RGB LED color. Use it before changing volume or brightness, or "
                      "to answer questions about the battery or the network.",
                      get_device_status);
    added += add_tool(mcp, "self.audio_speaker.set_volume", "Set the speaker volume (0 is silent, 100 is the loudest).",
                      set_volume, volume, 1);
    added += add_tool(mcp, "self.screen.set_brightness", "Set the screen brightness (1 is the dimmest, 100 the brightest).",
                      set_brightness, brightness, 1);
    added += add_tool(mcp, "self.led.set_color",
                      "Set the color of the RGB LED on the device. Each of red, green and blue is 0 to 255; "
                      "red=0, green=0, blue=0 switches the LED off.",
                      set_led_color, color, 3);
    ESP_LOGI(TAG, "%d tools registered", added);
    return added > 0;
}
