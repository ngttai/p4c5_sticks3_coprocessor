#include "phone_volume.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"

#define TAG "VOL"

namespace {

constexpr int STEP = 5;
constexpr uint32_t REPEAT_DELAY_MS = 400;   // held this long: start repeating
constexpr uint32_t REPEAT_MS = 125;
constexpr uint32_t HUD_SHOW_MS = 1500;
constexpr uint32_t HUD_FADE_MS = 200;
constexpr int HUD_W = 64;                   // vertical pill: % on top, bar, speaker icon at the bottom
constexpr int HUD_H = 236;
constexpr int BAR_H = 140;
constexpr int HUD_TOP = 48;                 // just under the 40 px status bar
constexpr int HUD_RIGHT = 12;

struct KeyEvent {
    int dir;        // +1 up, -1 down
    bool pressed;
};

QueueHandle_t s_queue;

/* LVGL objects, used with the display lock */
lv_obj_t *s_hud;
lv_obj_t *s_icon;
lv_obj_t *s_bar;
lv_obj_t *s_pct;
lv_timer_t *s_hide_timer;

int current_level()
{
    return phone_pref_get("mute", 0) ? 0 : phone_pref_get("vol", 60);
}

void hud_fade(int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_hud);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, HUD_FADE_MS);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

void hud_build()
{
    s_hud = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_hud);
    lv_obj_set_size(s_hud, HUD_W, HUD_H);
    lv_obj_align(s_hud, LV_ALIGN_TOP_RIGHT, -HUD_RIGHT, HUD_TOP);
    lv_obj_set_style_radius(s_hud, HUD_W / 2, 0);
    lv_obj_set_style_bg_opa(s_hud, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(s_hud, lv_color_hex(0x1E252C), 0);
    lv_obj_set_style_pad_ver(s_hud, 14, 0);
    lv_obj_set_style_pad_row(s_hud, 10, 0);
    lv_obj_set_flex_flow(s_hud, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_hud, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_hud, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    s_pct = lv_label_create(s_hud);
    lv_obj_set_style_text_color(s_pct, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pct, phone_font_by_size(16), 0);

    s_bar = lv_bar_create(s_hud);
    lv_obj_set_size(s_bar, 10, BAR_H);          // taller than wide: fills from the bottom
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A4652), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_INDICATOR);

    s_icon = lv_label_create(s_hud);
    lv_obj_set_style_text_color(s_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_icon, phone_font_by_size(20), 0);

    s_hide_timer = lv_timer_create([](lv_timer_t *t) {
        lv_timer_pause(t);
        hud_fade(LV_OPA_COVER, LV_OPA_TRANSP, [](lv_anim_t *) {
            lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        });
    }, HUD_SHOW_MS, nullptr);
    lv_timer_pause(s_hide_timer);
    lv_obj_add_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
}

/* LVGL context */
void hud_show(int level)
{
    if (!s_hud) {
        hud_build();
    }
    lv_label_set_text(s_icon, level == 0 ? LV_SYMBOL_MUTE : (level < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX));
    lv_bar_set_value(s_bar, level, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_pct, "%d", level);
    if (lv_obj_has_flag(s_hud, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_style_opa(s_hud, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(s_hud, LV_OBJ_FLAG_HIDDEN);
        hud_fade(LV_OPA_TRANSP, LV_OPA_COVER, nullptr);
    } else {
        lv_anim_delete(s_hud, nullptr);
        lv_obj_set_style_opa(s_hud, LV_OPA_COVER, 0);
    }
    lv_obj_move_foreground(s_hud);
    lv_timer_reset(s_hide_timer);
    lv_timer_resume(s_hide_timer);
    lv_display_trigger_activity(nullptr);   // also lifts a dimmed screen
}

/* Key task: one volume step, saved to NVS when the key is released */
int s_level;

void step(int dir)
{
    s_level = LV_CLAMP(0, s_level + dir * STEP, 100);
    phone_audio_set_volume(s_level);
    if (bsp_display_lock(100)) {
        hud_show(s_level);
        bsp_display_unlock();
    }
}

void save_level()
{
    if (s_level == 0) {
        phone_pref_set("mute", 1);       // keep the last volume for unmuting from Settings
    } else {
        phone_pref_set("vol", s_level);
        phone_pref_set("mute", 0);
    }
}

void key_task(void *)
{
    int held = 0;
    TickType_t next = 0;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (held) {
            const TickType_t now = xTaskGetTickCount();
            wait = next > now ? next - now : 0;
        }
        KeyEvent ev;
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            if (ev.pressed) {
                if (phone_lock_is_locked()) {
                    continue;            // nothing on the lock screen or with the screen off
                }
                held = ev.dir;
                if (held > 0 && phone_pref_get("mute", 0)) {
                    s_level = LV_MAX(phone_pref_get("vol", 60), STEP);   // unmute: back to the volume before muting
                    step(0);
                } else {
                    s_level = current_level();
                    step(held);
                }
                next = xTaskGetTickCount() + pdMS_TO_TICKS(REPEAT_DELAY_MS);
            } else if (held == ev.dir) {
                held = 0;
                save_level();
                phone_audio_beep();      // hear the new level once, not on every step
            }
        } else if (held) {
            step(held);
            next = xTaskGetTickCount() + pdMS_TO_TICKS(REPEAT_MS);
        }
    }
}

/* Button component callbacks (esp_timer task): just queue the event */
void on_key(void *, void *user)
{
    const intptr_t code = reinterpret_cast<intptr_t>(user);
    KeyEvent ev = {
        .dir = (code & 1) ? 1 : -1,
        .pressed = (code & 2) != 0,
    };
    xQueueSend(s_queue, &ev, 0);
}

} // namespace

extern "C" void phone_volume_start(void)
{
    if (s_queue) {
        return;
    }
    s_queue = xQueueCreate(8, sizeof(KeyEvent));
    button_handle_t btns[BSP_BUTTON_NUM];
    if (!s_queue || bsp_iot_button_create(btns, nullptr, BSP_BUTTON_NUM) != ESP_OK ||
            xTaskCreate(key_task, "vol_keys", 2048, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Volume keys unavailable");
        return;
    }
    /* user data: bit 0 = up, bit 1 = press (else release) */
    iot_button_register_cb(btns[BSP_BUTTON_IO0], BUTTON_PRESS_DOWN, nullptr, on_key, reinterpret_cast<void *>(3));
    iot_button_register_cb(btns[BSP_BUTTON_IO0], BUTTON_PRESS_UP, nullptr, on_key, reinterpret_cast<void *>(1));
    iot_button_register_cb(btns[BSP_BUTTON_BOOT], BUTTON_PRESS_DOWN, nullptr, on_key, reinterpret_cast<void *>(2));
    iot_button_register_cb(btns[BSP_BUTTON_BOOT], BUTTON_PRESS_UP, nullptr, on_key, reinterpret_cast<void *>(0));
}
