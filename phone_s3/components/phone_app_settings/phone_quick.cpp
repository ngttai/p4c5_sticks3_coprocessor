#include "phone_quick.h"
#include <ctime>
#include <vector>
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_log.h"
#include "lvgl_private.h"
#include "phone_app_settings.hpp"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_net.h"
#include "phone_power.h"

using namespace esp_brookesia::systems;

namespace {

const char *TAG = "quick";

#define T(vi, en) phone_tr(vi, en)

constexpr int PANEL_H = 376;
constexpr int TILE_Y = 84;
constexpr int TILE_H = 112;
constexpr int SLIDER_Y = TILE_Y + TILE_H + 16;
constexpr int TOP_ZONE = 40;            // a pull starts this close to the top edge
constexpr int HANDLE_ZONE = 70;         // a swipe up closes the panel from its last 70 px or below
constexpr int ANIM_MS = 200;
constexpr int BRI_MIN = 5;              // as the Settings slider

constexpr uint32_t COLOR_PANEL = 0x141A20;
constexpr uint32_t COLOR_TILE = 0x1E252C;
constexpr uint32_t COLOR_TILE_ON = 0x2F80ED;
constexpr uint32_t COLOR_FILL = 0x3A444E;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;

phone::Phone *s_phone;
lv_obj_t *s_shade;          // dims the screen under the panel, a tap closes
lv_obj_t *s_panel;
lv_obj_t *s_time;
lv_obj_t *s_date;
lv_obj_t *s_batt;
lv_obj_t *s_wifi_tile;
lv_obj_t *s_wifi_sub;
lv_obj_t *s_mute_tile;
lv_obj_t *s_mute_icon;
lv_obj_t *s_mute_sub;
lv_obj_t *s_bri;
lv_obj_t *s_vol;
lv_timer_t *s_timer;
bool s_dragging;
bool s_open;
bool s_swallow;             // the current gesture began with the panel out: hide it until its release

/* The shade dims the screen only once the panel is fully out: a dim that follows the finger
 * redraws and blends the whole 480 x 800 screen on every step (CPU near 100%, music stutters) */
void set_dim(bool on)
{
    lv_obj_set_style_bg_opa(s_shade, on ? LV_OPA_40 : LV_OPA_TRANSP, 0);
}

void set_y(int y)
{
    lv_obj_set_y(s_panel, LV_CLAMP(-PANEL_H, y, 0));
}

void anim_y(void *, int32_t v)
{
    set_y(v);
}

void open_done(lv_anim_t *)
{
    set_dim(true);
}

void hide_done(lv_anim_t *)
{
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_timer);
}

void slide(bool open)
{
    s_open = open;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, lv_obj_get_y(s_panel), open ? 0 : -PANEL_H);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, open ? open_done : hide_done);
    if (!open) {
        set_dim(false);
    }
    lv_anim_start(&a);
}

void close_panel()
{
    if (s_open || s_dragging) {
        s_dragging = false;
        slide(false);
    }
}

void refresh(lv_timer_t *)
{
    if (phone_lock_is_locked()) {
        close_panel();
        return;
    }
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (phone_lang_get() == PHONE_LANG_VI) {
        static const char *days[] = {"CN", "Th 2", "Th 3", "Th 4", "Th 5", "Th 6", "Th 7"};
        lv_label_set_text_fmt(s_date, "%s, %d/%d", days[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1);
    } else {
        char buf[24];
        strftime(buf, sizeof(buf), "%a, %d %b", &tm);
        lv_label_set_text(s_date, buf);
    }
    bsp_battery_info_t b;
    if (phone_power_get_battery(&b) && b.state != BSP_BATTERY_ABSENT) {
        lv_label_set_text_fmt(s_batt, "%s %d%%", b.state == BSP_BATTERY_CHARGING ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL,
                              b.percent);
    } else {
        lv_label_set_text(s_batt, "");
    }

    /* Wi-Fi: on/off, network name */
    phone_net_status_t st = {};
    phone_net_get_status(&st);
    const bool wifi = phone_net_is_enabled();
    lv_obj_set_style_bg_color(s_wifi_tile, lv_color_hex(wifi ? COLOR_TILE_ON : COLOR_TILE), 0);
    lv_label_set_text(s_wifi_sub, !wifi ? T("Tắt", "Off") : st.wifi_connected ? st.ssid
                      : st.wifi_connecting ? T("Đang kết nối…", "Connecting…") : T("Bật", "On"));

    /* Mute and the sliders follow the volume keys and Settings */
    const bool muted = phone_pref_get("mute", 0) != 0;
    lv_obj_set_style_bg_color(s_mute_tile, lv_color_hex(muted ? COLOR_TILE_ON : COLOR_TILE), 0);
    lv_obj_set_style_text_color(s_mute_icon, lv_color_hex(muted ? 0xFFFFFF : COLOR_SUB), 0);
    lv_label_set_text(s_mute_sub, muted ? T("Bật", "On") : T("Tắt", "Off"));
    if (!lv_obj_has_state(s_vol, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_vol, phone_audio_get_volume(), LV_ANIM_OFF);
    }
    if (!lv_obj_has_state(s_bri, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_bri, phone_pref_get("bri", 100), LV_ANIM_OFF);
    }
}

void show_start()
{
    set_dim(false);
    lv_obj_remove_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_shade);
    lv_anim_delete(s_panel, anim_y);
    lv_timer_resume(s_timer);
    refresh(nullptr);
}

/* ---- touch on the panel ---- */

enum class Open { MAIN, WIFI };

void open_settings(Open page)
{
    close_panel();
    if (page == Open::WIFI) {
        phone_settings_open_wifi_next();
    }
    base::Context::AppEventData ev = {
        .id = esp_brookesia::apps::PhoneSettings::requestInstance()->getId(),
        .type = base::Context::AppEventType::START,
        .data = nullptr,
    };
    s_phone->sendAppEvent(&ev);
}

void on_wifi(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        open_settings(Open::WIFI);
        return;
    }
    phone_net_set_enabled(!phone_net_is_enabled());
    refresh(nullptr);
}

void on_mute(lv_event_t *)
{
    const bool mute = phone_pref_get("mute", 0) == 0;
    phone_pref_set("mute", mute ? 1 : 0);
    if (mute) {
        phone_audio_set_volume(0);
    } else {
        phone_audio_set_volume(phone_pref_get("vol", 60));
        phone_audio_beep();
    }
    refresh(nullptr);
}

void on_bri(lv_event_t *e)
{
    const int v = lv_slider_get_value(s_bri);
    bsp_display_brightness_set(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        phone_pref_set("bri", v);
    }
}

void on_vol(lv_event_t *e)
{
    const int v = lv_slider_get_value(s_vol);
    phone_audio_set_volume(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        if (v == 0) {
            phone_pref_set("mute", 1);
        } else {
            phone_pref_set("vol", v);
            phone_pref_set("mute", 0);
            phone_audio_beep();
        }
        refresh(nullptr);
    }
}

/* ---- pull down from the top edge, swipe up to close ---- */

/* A new gesture starts. s_swallow is cleared by the release event of the gesture that set it; if that
 * release never reached us, every later gesture would stay hidden from the launcher and the apps
 * (swipes dead, taps and the pull-down still working): clear it at the next start */
void on_gesture_press(lv_event_t *)
{
    if (s_swallow && !s_open && !s_dragging) {
        ESP_LOGW(TAG, "gesture swallow left set by the last gesture: cleared");
        s_swallow = false;
    }
}

void on_gesture(lv_event_t *e)
{
    auto *g = s_phone->getManager().getGesture();
    auto *info = static_cast<phone::Gesture::Info *>(lv_event_get_param(e));
    if (!info) {
        return;
    }
    const bool release = lv_event_get_code(e) == g->getReleaseEventCode();
    /* While the panel is out, its sliders and tiles take the touch: the launcher (page swipes),
     * the navigation and apps listening to gestures (2048) must not see them. A tap on the shade
     * closes the panel on the LVGL release, before this release event: the whole gesture stays hidden */
    if (s_dragging || s_open) {
        s_swallow = true;
    }
    if (s_swallow) {
        lv_event_stop_processing(e);
        if (release) {
            s_swallow = false;
        }
    }
    if (s_dragging) {
        if (!release) {
            set_y(info->stop_y - PANEL_H);
        } else {
            s_dragging = false;
            slide(lv_obj_get_y(s_panel) > -PANEL_H * 2 / 3 || info->direction == phone::Gesture::DIR_DOWN);
        }
        return;
    }
    if (!s_open) {
        if (!release && info->start_y < TOP_ZONE && info->direction == phone::Gesture::DIR_DOWN &&
                !phone_lock_is_locked()) {
            s_dragging = true;
            s_swallow = true;
            lv_event_stop_processing(e);
            show_start();
            set_y(info->stop_y - PANEL_H);
        }
        return;
    }
    if (release && info->direction == phone::Gesture::DIR_UP && info->start_y > PANEL_H - HANDLE_ZONE) {
        close_panel();
    }
}

/* ---- widgets ---- */

lv_obj_t *make_tile(lv_obj_t *parent, int x, const char *icon, const char *title, lv_obj_t **icon_out, lv_obj_t **sub_out,
                    lv_event_cb_t cb, bool long_press, int y = TILE_Y)
{
    lv_obj_t *t = lv_button_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, 214, TILE_H);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_radius(t, 20, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(COLOR_TILE), 0);
    lv_obj_set_style_pad_all(t, 16, 0);
    lv_obj_add_event_cb(t, cb, LV_EVENT_SHORT_CLICKED, nullptr);
    if (long_press) {
        lv_obj_add_event_cb(t, cb, LV_EVENT_LONG_PRESSED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(t);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_label_set_text(ic, icon);
    lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *ti = lv_label_create(t);
    lv_label_set_text(ti, title);
    lv_obj_align(ti, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_t *sub = lv_label_create(t);
    lv_obj_set_width(sub, 182);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(sub, phone_font_by_size(16), 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0xDDE3E9), 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    if (icon_out) {
        *icon_out = ic;
    }
    *sub_out = sub;
    return t;
}

lv_obj_t *make_slider(lv_obj_t *parent, int y, const char *icon, int min, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, 440, 56);
    lv_obj_set_pos(s, 20, y);
    lv_slider_set_range(s, min, 100);
    lv_obj_set_style_radius(s, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_TILE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 20, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_FILL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, nullptr);
    if (icon) {
        lv_obj_t *ic = lv_label_create(s);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(ic, lv_color_white(), 0);
        lv_label_set_text(ic, icon);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 16, 0);
    }
    return s;
}

/* A sun for the brightness slider (the symbol font has none): a disc and eight rays */
void draw_sun(lv_obj_t *slider)
{
    lv_obj_t *box = lv_obj_create(slider);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 28, 28);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    auto dot = [box](int x, int y, int d) {
        lv_obj_t *o = lv_obj_create(box);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, d, d);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_white(), 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    };
    dot(8, 8, 12);
    for (int i = 0; i < 8; i++) {
        static const int8_t pos[8][2] = {{12, 0}, {21, 3}, {24, 12}, {21, 21}, {12, 24}, {3, 21}, {0, 12}, {3, 3}};
        dot(pos[i][0], pos[i][1], 4);
    }
}

void build()
{
    lv_obj_t *layer = lv_layer_sys();      /* over the status bar: the panel shows the time and battery */
    s_shade = lv_obj_create(layer);
    lv_obj_remove_style_all(s_shade);
    lv_obj_set_size(s_shade, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_shade, lv_color_black(), 0);
    lv_obj_add_flag(s_shade, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(s_shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_shade, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == s_shade) {
            close_panel();
        }
    }, LV_EVENT_CLICKED, nullptr);

    s_panel = lv_obj_create(s_shade);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_PCT(100), PANEL_H);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_radius(s_panel, 24, 0);
    lv_obj_set_style_text_color(s_panel, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_panel, phone_font_by_size(20), 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);       // a tap on the panel does not close it
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_y(s_panel, -PANEL_H);

    /* header: time, date, battery, Settings */
    s_time = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_time, phone_font_by_size(28), 0);
    lv_obj_set_pos(s_time, 24, 24);
    s_date = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_date, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_date, phone_font_by_size(16), 0);
    lv_obj_set_pos(s_date, 112, 34);
    s_batt = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_16, 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -84, 34);
    lv_obj_t *gear = lv_button_create(s_panel);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 48, 48);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(COLOR_TILE), 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(COLOR_FILL), LV_STATE_PRESSED);
    lv_obj_add_event_cb(gear, [](lv_event_t *) {
        open_settings(Open::MAIN);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *gl = lv_label_create(gear);
    lv_obj_set_style_text_font(gl, &lv_font_montserrat_24, 0);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);

    s_wifi_tile = make_tile(s_panel, 20, LV_SYMBOL_WIFI, "Wi-Fi", nullptr, &s_wifi_sub, on_wifi, true);
    s_mute_tile = make_tile(s_panel, 246, LV_SYMBOL_MUTE, T("Im lặng", "Silent"), &s_mute_icon, &s_mute_sub, on_mute, false);
    s_bri = make_slider(s_panel, SLIDER_Y, nullptr, BRI_MIN, on_bri);
    draw_sun(s_bri);
    s_vol = make_slider(s_panel, SLIDER_Y + 68, LV_SYMBOL_VOLUME_MAX, 0, on_vol);

    lv_obj_t *handle = lv_obj_create(s_panel);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 48, 5);
    lv_obj_set_style_radius(handle, 3, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x4A5560), 0);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -14);

    s_timer = lv_timer_create(refresh, 1000, nullptr);
    lv_timer_pause(s_timer);
}

} // namespace

bool phone_quick_visible(void)
{
    return s_shade && !lv_obj_has_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
}

void phone_quick_start(phone::Phone *phone)
{
    if (s_phone) {
        return;
    }
    s_phone = phone;
    build();
    /* Our handler goes first among the gesture listeners, so that it can hide the gestures made on
     * the panel from the ones added by the system (apps add theirs later, after it anyway) */
    auto *g = phone->getManager().getGesture();
    lv_obj_t *obj = g->getEventObj();
    struct Listener {
        lv_event_cb_t cb;
        uint32_t filter;
        void *user_data;
    };
    std::vector<Listener> others;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        others.push_back({d->cb, d->filter, d->user_data});
    }
    while (lv_obj_get_event_count(obj)) {
        lv_obj_remove_event(obj, 0);
    }
    lv_obj_add_event_cb(obj, on_gesture_press, g->getPressEventCode(), nullptr);
    lv_obj_add_event_cb(obj, on_gesture, g->getPressingEventCode(), nullptr);
    lv_obj_add_event_cb(obj, on_gesture, g->getReleaseEventCode(), nullptr);
    for (const auto &l : others) {
        lv_obj_add_event_cb(obj, l.cb, static_cast<lv_event_code_t>(l.filter), l.user_data);
    }
}
