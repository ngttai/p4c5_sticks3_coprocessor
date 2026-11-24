#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_hosted_host_fw_ver.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "SETT"
#include "esp_lib_utils.h"
#include "phone_fonts.h"
#include "phone_net.h"
#include "phone_power.h"
#include "phone_locale.h"
#include "phone_audio.h"
#include "phone_storage.h"
#include "phone_monitor.h"
#include "phone_speedtest.h"
#include "phone_membench.h"
#include "phone_sdbench.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "phone_lock.h"
#include "phone_app_settings.hpp"

#define APP_NAME "Cài đặt"
#define TAG "SETT"
#define T(vi, en) phone_tr(vi, en)

extern "C" const lv_image_dsc_t phone_settings_icon;

using namespace esp_brookesia::systems;

namespace {

enum Page {
    PAGE_MAIN,
    PAGE_WIFI,
    PAGE_PASS,
    PAGE_DISPLAY,
    PAGE_SOUND,
    PAGE_LANG,
    PAGE_TZ,
    PAGE_POWER,
    PAGE_ABOUT,
    PAGE_SPEED,
    PAGE_MEM,
    PAGE_COUNT,
};

enum WifiState {
    WIFI_IDLE,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_FAILED,
};

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_ACCENT = 0x2F80ED;
constexpr uint32_t COLOR_TEXT = 0xFFFFFF;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr int NAV_STRIP_H = 36;     // bottom strip where the swipe-up for the navigation bar starts

lv_obj_t *s_pages[PAGE_COUNT];
int s_cur_page = PAGE_MAIN;

lv_obj_t *s_wifi_status;
lv_obj_t *s_wifi_list;
lv_obj_t *s_wifi_body;
lv_obj_t *s_wifi_list_cap;
lv_obj_t *s_pass_title;
lv_obj_t *s_pass_ta;
lv_obj_t *s_about_vals[5];
lv_obj_t *s_c5_ver_val;
lv_obj_t *s_link_vals[3];            /* S3 link: I2S frames, lost frames, playback underruns */
lv_obj_t *s_sd_val;
lv_obj_t *s_to_check[8];
lv_obj_t *s_batt_vals[6];
lv_obj_t *s_bri_val;
lv_obj_t *s_bri_slider;
lv_obj_t *s_vol_val;
lv_obj_t *s_vol_slider;
lv_obj_t *s_mute_sw;
bool s_muted;
lv_obj_t *s_wifi_sw;
bool s_wifi_on = true;
bool s_open_wifi;          // show the Wi-Fi page on the next run or resume
lv_obj_t *s_kb;

bool s_wifi_inited;
std::atomic<int> s_wifi_state{WIFI_IDLE};
std::atomic<bool> s_scan_done{false};
lv_obj_t *s_scan_icon = nullptr;

void scan_spin_exec(void *obj, int32_t v)
{
    lv_obj_set_style_transform_rotation(static_cast<lv_obj_t *>(obj), v, 0);
}

void scan_spin_start()
{
    if (!s_scan_icon) {
        return;
    }
    lv_obj_update_layout(s_scan_icon);
    lv_obj_set_style_transform_pivot_x(s_scan_icon, lv_obj_get_width(s_scan_icon) / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_scan_icon, lv_obj_get_height(s_scan_icon) / 2, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_scan_icon);
    lv_anim_set_exec_cb(&a, scan_spin_exec);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void scan_spin_stop()
{
    if (!s_scan_icon) {
        return;
    }
    lv_anim_delete(s_scan_icon, scan_spin_exec);
    lv_obj_set_style_transform_rotation(s_scan_icon, 0, 0);
}
char s_ip[16];
char s_ssid[33];
std::vector<wifi_ap_record_t> s_aps;
char s_sel_ssid[33];
bool s_sel_open;
bool s_connect_requested = false;

void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            s_scan_done = true;
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_wifi_state = (s_wifi_state == WIFI_CONNECTING && s_connect_requested) ? WIFI_FAILED : WIFI_IDLE;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_state = WIFI_CONNECTED;
    }
}

void wifi_sync_state()
{
    phone_net_status_t st;
    phone_net_get_status(&st);
    strlcpy(s_ssid, st.ssid, sizeof(s_ssid));
    strlcpy(s_ip, st.ip, sizeof(s_ip));
    s_wifi_state = st.wifi_connected ? WIFI_CONNECTED : (st.wifi_connecting ? WIFI_CONNECTING : WIFI_IDLE);
}

void wifi_init_once()
{
    if (s_wifi_inited) {
        return;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, nullptr);
    s_wifi_inited = true;
}

void set_list_empty(bool empty)
{
    if (s_wifi_list) {
        lv_obj_set_height(s_wifi_list, empty ? 14 : LV_SIZE_CONTENT);
    }
}

void wifi_scan()
{
    if (!s_wifi_on) {
        return;
    }
    lv_obj_clean(s_wifi_list);
    set_list_empty(true);
    if (!phone_net_ready()) {
        lv_label_set_text(s_wifi_status, T("Wi-Fi đang khởi tạo…", "Wi-Fi is starting…"));
        return;
    }
    wifi_init_once();
    wifi_sync_state();
    if (esp_wifi_scan_start(nullptr, false) == ESP_OK) {
        scan_spin_start();
    }
}

void wifi_connect(const char *ssid, const char *pass)
{
    s_connect_requested = true;
    s_wifi_state = WIFI_CONNECTING;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    phone_net_connect(ssid, pass);
}

bool is_connected_ssid(const char *ssid)
{
    return s_wifi_state.load() == WIFI_CONNECTED && strcmp(ssid, s_ssid) == 0;
}

void connect_saved(int idx)
{
    char ssid[33] = {};
    if (!phone_net_saved_at(idx, ssid, sizeof(ssid)) || is_connected_ssid(ssid)) {
        return;
    }
    s_connect_requested = true;
    s_wifi_state = WIFI_CONNECTING;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    phone_net_connect_saved(idx);
}

lv_obj_t *s_eye_lbl = nullptr;
lv_obj_t *s_saved_caption = nullptr;
lv_obj_t *s_saved_card = nullptr;
lv_obj_t *s_saved_list = nullptr;
char s_saved_sig[256] = {};
lv_obj_t *s_confirm = nullptr;
int s_confirm_idx = -1;
bool s_pass_visible = false;

void set_pass_visible(bool visible)
{
    s_pass_visible = visible;
    lv_textarea_set_password_mode(s_pass_ta, !visible);
    lv_label_set_text(s_eye_lbl, visible ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

void on_eye_toggle(lv_event_t *e)
{
    set_pass_visible(!s_pass_visible);
}

void on_kb_cancel(lv_event_t *e)
{
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

void on_pass_click(lv_event_t *e)
{
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
}

void show_page(int page)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_pages[i]) {
            if (i == page) {
                lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_kb) {
        if (page == PAGE_PASS) {
            lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_cur_page = page;
}

lv_obj_t *make_page(lv_obj_t *root, const char *title, int page)
{
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(p, 16, 0);
    lv_obj_set_style_pad_bottom(p, 16 + NAV_STRIP_H, 0);    /* the last row scrolls clear of the strip */
    lv_obj_set_style_pad_row(p, 12, 0);
    /* vertical scrolling with a thin bar on the right that shows while the page moves */
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(p, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(p, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(p, lv_color_hex(COLOR_TEXT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(p, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(p, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(p, 8, LV_PART_SCROLLBAR);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *t = lv_label_create(p);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, phone_font_by_size(28), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), 0);
    s_pages[page] = p;
    return p;
}

lv_obj_t *make_row(lv_obj_t *parent, const char *text, const char *sub, lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 64);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
    if (sub) {
        lv_obj_t *r = lv_label_create(row);
        lv_label_set_text(r, sub);
        lv_obj_set_style_text_color(r, lv_color_hex(COLOR_SUB), 0);
    }
    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    }
    return row;
}

lv_obj_t *make_text(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void on_open_page(lv_event_t *e);

lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *make_caption(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_pad_left(cap, 12, 0);
    lv_obj_set_style_margin_top(cap, 8, 0);
    return cap;
}

lv_obj_t *make_group(lv_obj_t *parent, const char *caption)
{
    make_caption(parent, caption);
    return make_card(parent);
}

lv_obj_t *grouped_row(lv_obj_t *card, const char *text, lv_event_cb_t cb, void *user)
{
    lv_obj_t *row = make_row(card, text, nullptr, cb, user);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COLOR_BG), 0);
    return row;
}

const char kIconInfo[] = "@info";
const char kIconGlobe[] = "@globe";
const char kIconBug[] = "@bug";
const char kIconChart[] = "@chart";

lv_obj_t *glyph_part(lv_obj_t *parent, int x, int y, int w, int h, int radius, int border, bool fill)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    if (fill) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(COLOR_TEXT), 0);
    } else {
        lv_obj_set_style_border_width(o, border, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(COLOR_TEXT), 0);
    }
    return o;
}

void draw_info_glyph(lv_obj_t *tile)
{
    glyph_part(tile, 6, 6, 32, 32, LV_RADIUS_CIRCLE, 3, false);
    glyph_part(tile, 19, 11, 4, 4, 2, 0, true);
    glyph_part(tile, 19, 18, 4, 12, 2, 0, true);
}

void draw_globe_glyph(lv_obj_t *tile)
{
    glyph_part(tile, 7, 7, 30, 30, LV_RADIUS_CIRCLE, 2, false);
    glyph_part(tile, 16, 7, 12, 30, LV_RADIUS_CIRCLE, 2, false);
    glyph_part(tile, 7, 21, 30, 2, 0, 0, true);
}

/* A bug: head, body with a split down the back, three legs a side, two antennae */
void draw_bug_glyph(lv_obj_t *tile, uint32_t tile_color)
{
    glyph_part(tile, 17, 9, 10, 8, LV_RADIUS_CIRCLE, 0, true);          // head
    glyph_part(tile, 14, 15, 16, 21, LV_RADIUS_CIRCLE, 0, true);        // body
    lv_obj_t *split = glyph_part(tile, 21, 18, 2, 16, 0, 0, true);
    lv_obj_set_style_bg_color(split, lv_color_hex(tile_color), 0);
    for (int y : {18, 24, 30}) {
        glyph_part(tile, 8, y, 7, 2, 1, 0, true);
        glyph_part(tile, 29, y, 7, 2, 1, 0, true);
    }
    glyph_part(tile, 15, 5, 2, 6, 1, 0, true);                          // antennae
    glyph_part(tile, 27, 5, 2, 6, 1, 0, true);
}

/* Three bars of a chart, for the system monitor */
void draw_chart_glyph(lv_obj_t *tile)
{
    glyph_part(tile, 9, 24, 7, 12, 2, 0, true);
    glyph_part(tile, 18, 12, 7, 24, 2, 0, true);
    glyph_part(tile, 28, 19, 7, 17, 2, 0, true);
}

/* Moves the label of a grouped row next to a colored icon tile on the left */
void add_tile(lv_obj_t *row, const char *symbol, uint32_t tile_color)
{
    lv_obj_t *label = lv_obj_get_child(row, 0);
    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 14, 0);
    lv_obj_set_parent(label, left);
    lv_obj_t *tile = lv_obj_create(left);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 44, 44);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(tile_color), 0);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    if (strcmp(symbol, kIconInfo) == 0) {
        draw_info_glyph(tile);
    } else if (strcmp(symbol, kIconGlobe) == 0) {
        draw_globe_glyph(tile);
    } else if (strcmp(symbol, kIconBug) == 0) {
        draw_bug_glyph(tile, tile_color);
    } else if (strcmp(symbol, kIconChart) == 0) {
        draw_chart_glyph(tile);
    } else {
        lv_obj_t *ic = lv_label_create(tile);
        lv_label_set_text(ic, symbol);
        lv_obj_set_style_text_color(ic, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_center(ic);
    }
    lv_obj_move_to_index(tile, 0);
    lv_obj_move_to_index(left, 0);
}

void group_row(lv_obj_t *card, const char *symbol, uint32_t tile_color, const char *text, const char *value, int page)
{
    lv_obj_t *row = grouped_row(card, text, on_open_page, reinterpret_cast<void *>(static_cast<intptr_t>(page)));
    add_tile(row, symbol, tile_color);
    lv_obj_t *r = lv_label_create(row);
    lv_label_set_text_fmt(r, "%s  " LV_SYMBOL_RIGHT, value ? value : "");
    lv_obj_set_style_text_color(r, lv_color_hex(COLOR_SUB), 0);
}

lv_obj_t *kv_row(lv_obj_t *card, const char *key)
{
    lv_obj_t *row = grouped_row(card, key, nullptr, nullptr);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, lv_color_hex(COLOR_SUB), 0);
    return v;
}

lv_obj_t *switch_row(lv_obj_t *card, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *row = grouped_row(card, text, nullptr, nullptr);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_ACCENT), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

void group_end(lv_obj_t *card)
{
    uint32_t n = lv_obj_get_child_count(card);
    if (n) {
        lv_obj_set_style_border_width(lv_obj_get_child(card, n - 1), 0, 0);
    }
}

void stop_auto_connect()
{
    /* The app's own state is stale until the Wi-Fi page has been open: ask the service first,
     * otherwise an established link would be dropped here */
    wifi_sync_state();
    if (s_wifi_state.load() == WIFI_CONNECTED) {
        return;
    }
    s_connect_requested = false;
    s_wifi_state = WIFI_IDLE;
    phone_net_pause_auto();     /* only the attempt: Wi-Fi stays on (phone_net_disconnect() is the switch) */
}

void refresh_wifi_status();

void refresh_wifi_status();

void close_confirm()
{
    if (s_confirm) {
        lv_obj_delete(s_confirm);
        s_confirm = nullptr;
    }
}

void forget_ssid_now(const char *ssid)
{
    auto st = s_wifi_state.load();
    bool active = (st == WIFI_CONNECTED || st == WIFI_CONNECTING) && strcmp(ssid, s_ssid) == 0;
    phone_net_forget_ssid(ssid);
    if (active) {
        s_connect_requested = false;
        s_wifi_state = WIFI_IDLE;
        s_ssid[0] = '\0';
    }
    refresh_wifi_status();
}

void on_confirm_cancel(lv_event_t *e)
{
    close_confirm();
}

void on_confirm_ok(lv_event_t *e)
{
    char ssid[33] = {};
    if (s_confirm_idx >= 0 && phone_net_saved_at(s_confirm_idx, ssid, sizeof(ssid))) {
        forget_ssid_now(ssid);
    }
    close_confirm();
}

lv_obj_t *make_menu_btn(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t color, lv_event_cb_t cb);

void show_forget_confirm(int idx)
{
    close_confirm();
    s_confirm_idx = idx;
    char ssid[33] = {};
    if (!phone_net_saved_at(idx, ssid, sizeof(ssid))) {
        return;
    }

    s_confirm = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_style_text_font(s_confirm, phone_font_by_size(20), 0);
    lv_obj_set_size(s_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_60, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = lv_obj_create(s_confirm);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(88));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(box, 28, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, T("Quên mạng này?", "Forget this network?"));
    lv_obj_set_style_text_font(title, phone_font_by_size(24), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);

    lv_obj_t *name = lv_label_create(box);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, ssid);
    lv_obj_set_style_text_font(name, phone_font_by_size(20), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_SUB), 0);

    make_menu_btn(box, T("Quên mạng", "Forget network"), 0x2A333C, 0xFF453A, on_confirm_ok);
    make_menu_btn(box, T("Hủy", "Cancel"), 0x2A333C, COLOR_SUB, on_confirm_cancel);
}

void on_menu_connect(lv_event_t *e)
{
    int idx = s_confirm_idx;
    close_confirm();
    connect_saved(idx);
}

void on_menu_forget(lv_event_t *e)
{
    show_forget_confirm(s_confirm_idx);
}

lv_obj_t *make_menu_btn(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_PCT(100), 56);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(btn, 28, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A444E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUSED | LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_center(l);
    return btn;
}

void show_saved_menu(int idx)
{
    close_confirm();
    s_confirm_idx = idx;
    char ssid[33] = {};
    if (!phone_net_saved_at(idx, ssid, sizeof(ssid))) {
        return;
    }

    s_confirm = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_confirm);
    lv_obj_set_style_text_font(s_confirm, phone_font_by_size(20), 0);
    lv_obj_set_size(s_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_60, 0);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *box = lv_obj_create(s_confirm);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(88));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(box, 28, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);

    lv_obj_t *name = lv_label_create(box);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, ssid);
    lv_obj_set_style_text_font(name, phone_font_by_size(20), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_SUB), 0);

    lv_obj_t *conn = make_menu_btn(box, T("Kết nối", "Connect"), 0x2A333C, COLOR_TEXT, on_menu_connect);
    if (is_connected_ssid(ssid)) {
        lv_obj_remove_flag(conn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(conn, LV_OPA_40, 0);
    }
    make_menu_btn(box, T("Quên mạng", "Forget network"), 0x2A333C, 0xFF453A, on_menu_forget);
    make_menu_btn(box, T("Hủy", "Cancel"), 0x2A333C, COLOR_SUB, on_confirm_cancel);
}

void on_saved_menu(lv_event_t *e)
{
    show_saved_menu(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void on_saved_pick(lv_event_t *e);

void add_saved_row(const char *ssid, bool current, int idx)
{
    lv_obj_t *row = lv_obj_create(s_saved_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 14, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x2A333C), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_flex_grow(lbl, 1);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(lbl, ssid);
    lv_obj_set_style_text_color(lbl, lv_color_hex(current ? 0x34C759 : COLOR_SUB), 0);
    lv_obj_set_style_text_font(lbl, phone_font_by_size(20), 0);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl, on_saved_pick, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(idx)));


    lv_obj_t *btn = lv_obj_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 64, 48);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(btn, 24, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ROW), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_saved_menu, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(idx)));
    lv_obj_t *fl = lv_label_create(btn);
    lv_label_set_text(fl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(fl, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(fl);
}

void refresh_saved()
{
    int count = phone_net_saved_count();
    if (!s_wifi_on || !s_saved_list) {
        lv_obj_add_flag(s_saved_caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_saved_card, LV_OBJ_FLAG_HIDDEN);
        s_saved_sig[0] = '\0';
        return;
    }
    if (count < 0) {
        count = 0;
    }
    lv_obj_remove_flag(s_saved_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_saved_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_saved_card, count > 0 ? LV_SIZE_CONTENT : 14);
    lv_obj_set_style_radius(s_saved_card, 8, 0);

    bool connected = s_wifi_state.load() == WIFI_CONNECTED;
    char sig[sizeof(s_saved_sig)] = {};
    for (int i = 0; i < count; i++) {
        char ssid[33] = {};
        if (!phone_net_saved_at(i, ssid, sizeof(ssid))) {
            continue;
        }
        bool current = connected && strcmp(ssid, s_ssid) == 0;
        size_t used = strlen(sig);
        snprintf(sig + used, sizeof(sig) - used, "%s%s%s;", ssid, current ? "*" : "", current ? s_ip : "");
    }
    if (strcmp(sig, s_saved_sig) == 0) {
        return;
    }
    strlcpy(s_saved_sig, sig, sizeof(s_saved_sig));
    lv_obj_clean(s_saved_list);
    for (int i = 0; i < count; i++) {
        char ssid[33] = {};
        if (!phone_net_saved_at(i, ssid, sizeof(ssid))) {
            continue;
        }
        add_saved_row(ssid, connected && strcmp(ssid, s_ssid) == 0, i);
    }
    uint32_t n = lv_obj_get_child_count(s_saved_list);
    if (n) {
        lv_obj_set_style_border_width(lv_obj_get_child(s_saved_list, n - 1), 0, 0);
    }
}

void on_open_page(lv_event_t *e)
{
    int page = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (page == PAGE_WIFI) {
        stop_auto_connect();
        wifi_scan();
    }
    show_page(page);
}

void on_scan(lv_event_t *e)
{
    stop_auto_connect();
    wifi_scan();
}

void on_pick_ap(lv_event_t *e)
{
    size_t idx = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx >= s_aps.size()) {
        return;
    }
    const wifi_ap_record_t &ap = s_aps[idx];
    strlcpy(s_sel_ssid, reinterpret_cast<const char *>(ap.ssid), sizeof(s_sel_ssid));
    s_sel_open = (ap.authmode == WIFI_AUTH_OPEN);
    if (s_sel_open) {
        wifi_connect(s_sel_ssid, "");
        return;
    }
    lv_label_set_text(s_pass_title, s_sel_ssid);
    lv_textarea_set_text(s_pass_ta, "");
    set_pass_visible(false);
    show_page(PAGE_PASS);
}

void on_pass_ready(lv_event_t *e)
{
    wifi_connect(s_sel_ssid, lv_textarea_get_text(s_pass_ta));
    show_page(PAGE_WIFI);
}

void on_saved_pick(lv_event_t *e)
{
    connect_saved(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void on_power_pick(lv_event_t *e)
{
    phone_power_confirm(static_cast<phone_power_action_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void to_update_checks()
{
    int cur = phone_lock_timeout_get();
    for (int i = 0; i < phone_lock_timeout_count(); i++) {
        lv_label_set_text(s_to_check[i], i == cur ? LV_SYMBOL_OK : "");
    }
}

void on_timeout_pick(lv_event_t *e)
{
    phone_lock_timeout_set(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    to_update_checks();
}

void on_brightness(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    bsp_display_brightness_set(v);
    if (s_bri_val) {
        lv_label_set_text_fmt(s_bri_val, "%d%%", v);
    }
}

void on_volume(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    if (s_muted) {
        s_muted = false;
        lv_obj_remove_state(s_mute_sw, LV_STATE_CHECKED);
        phone_pref_set("mute", 0);
    }
    phone_audio_set_volume(v);
    lv_label_set_text_fmt(s_vol_val, "%d%%", v);
}

void on_volume_done(lv_event_t *e)
{
    phone_pref_set("vol", lv_slider_get_value(lv_event_get_target_obj(e)));
    phone_audio_beep();
}

void on_mute_toggle(lv_event_t *e)
{
    s_muted = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    phone_pref_set("mute", s_muted ? 1 : 0);
    if (s_muted) {
        phone_audio_set_volume(0);
        lv_slider_set_value(s_vol_slider, 0, LV_ANIM_ON);   /* the last volume stays in "vol" for unmuting */
        lv_label_set_text(s_vol_val, T("Tắt tiếng", "Muted"));
    } else {
        int v = phone_pref_get("vol", 60);
        phone_audio_set_volume(v);
        lv_slider_set_value(s_vol_slider, v, LV_ANIM_ON);
        lv_label_set_text_fmt(s_vol_val, "%d%%", v);
        phone_audio_beep();
    }
}

void refresh_wifi_status();

void on_wifi_toggle(lv_event_t *e)
{
    s_wifi_on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    if (s_wifi_body) {
        if (s_wifi_on) {
            lv_obj_remove_flag(s_wifi_body, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_wifi_on) {
        phone_net_set_enabled(true);
        s_wifi_state = WIFI_IDLE;
        wifi_scan();
    } else {
        phone_net_set_enabled(false);
        set_list_empty(true);
        s_wifi_state = WIFI_IDLE;
    }
    refresh_wifi_status();
}

void on_brightness_done(lv_event_t *e)
{
    phone_pref_set("bri", lv_slider_get_value(lv_event_get_target_obj(e)));
}

void refresh_wifi_list()
{
    /* Fetch every record (the C5 frees its list on this call), then keep the strongest 20 */
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    s_aps.assign(n, wifi_ap_record_t{});
    if (n == 0 || esp_wifi_scan_get_ap_records(&n, s_aps.data()) != ESP_OK) {
        n = 0;
    }
    s_aps.resize(n > 20 ? 20 : n);

    lv_obj_clean(s_wifi_list);
    for (size_t i = 0; i < s_aps.size(); i++) {
        char sub[24];
        snprintf(sub, sizeof(sub), "%d dBm", s_aps[i].rssi);
        const char *name = reinterpret_cast<const char *>(s_aps[i].ssid);
        lv_obj_t *row = grouped_row(s_wifi_list, name[0] ? name : T("(ẩn)", "(hidden)"), on_pick_ap, reinterpret_cast<void *>(i));
        lv_obj_t *r = lv_label_create(row);
        lv_label_set_text(r, sub);
        lv_obj_set_style_text_color(r, lv_color_hex(COLOR_SUB), 0);
    }
    if (!s_aps.empty()) {
        group_end(s_wifi_list);
    }
    set_list_empty(s_aps.empty());
}

void refresh_wifi_status()
{
    char buf[96];
    if (!s_wifi_on) {
        lv_label_set_text(s_wifi_status, T("Wi-Fi đã tắt", "Wi-Fi is off"));
        return;
    }
    // Follow the Wi-Fi service for auto connect/disconnect; a user connect in progress keeps its own state.
    if (s_wifi_state.load() != WIFI_CONNECTING) {
        phone_net_status_t st;
        phone_net_get_status(&st);
        if (st.wifi_connected) {
            strlcpy(s_ssid, st.ssid, sizeof(s_ssid));
            strlcpy(s_ip, st.ip, sizeof(s_ip));
            s_wifi_state = WIFI_CONNECTED;
        } else if (s_wifi_state.load() == WIFI_CONNECTED) {
            s_ssid[0] = '\0';
            s_ip[0] = '\0';
            s_wifi_state = WIFI_IDLE;
        }
    }
    switch (s_wifi_state.load()) {
    case WIFI_CONNECTING:
        snprintf(buf, sizeof(buf), "%s", T("Đang kết nối…", "Connecting…"));
        break;
    case WIFI_CONNECTED:
        snprintf(buf, sizeof(buf), "%s: %s  (%s)", T("Đã kết nối", "Connected"), s_ssid, s_ip);
        break;
    case WIFI_FAILED:
        snprintf(buf, sizeof(buf), "%s", T("Kết nối thất bại", "Connection failed"));
        break;
    default:
        snprintf(buf, sizeof(buf), "%s", T("Chưa kết nối", "Not connected"));
        break;
    }
    lv_label_set_text(s_wifi_status, buf);
}

/* One I2C address probe on the BSP bus; the bus is shared with touch, the driver serializes it. */
bool i2c_present(uint16_t addr)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    return bus && i2c_master_probe(bus, addr, 50) == ESP_OK;
}

void hw_row(lv_obj_t *card, const char *name, const char *chip, uint16_t addr)
{
    lv_obj_t *v = kv_row(card, name);
    char buf[48];
    if (addr) {
        snprintf(buf, sizeof(buf), "%s  %s", chip, i2c_present(addr) ? LV_SYMBOL_OK : T("không thấy", "not found"));
    } else {
        snprintf(buf, sizeof(buf), "%s", chip);
    }
    lv_label_set_text(v, buf);
}

void sw_row(lv_obj_t *card, const char *name, const char *value)
{
    lv_label_set_text(kv_row(card, name), value);
}

void refresh_about()
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    char buf[32];
    snprintf(buf, sizeof(buf), "P4 rev v%d.%d", chip.revision / 100, chip.revision % 100);
    lv_label_set_text(s_about_vals[0], buf);
    /* Hardware sizes; the free and usable amounts are in the system monitor (Debug).
     * ESP-IDF has no call for the physical SRAM: 768 KB HP L2MEM of the ESP32-P4 datasheet */
    lv_label_set_text(s_about_vals[1], "768 KB");
    snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(esp_psram_get_size() / (1024 * 1024)));
    lv_label_set_text(s_about_vals[2], buf);
    uint32_t flash = 0;
    if (esp_flash_get_size(nullptr, &flash) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(flash / (1024 * 1024)));
    } else {
        strlcpy(buf, "--", sizeof(buf));
    }
    lv_label_set_text(s_about_vals[3], buf);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_label_set_text(s_about_vals[4], buf);
    if (!phone_net_c5_version(buf, sizeof(buf))) {
        strlcpy(buf, "--", sizeof(buf));
    }
    lv_label_set_text(s_c5_ver_val, buf);
    {
        unsigned frames = 0, lost = 0, under = 0;
        bsp_audio_link_stats(&frames, &lost, &under, nullptr);
        snprintf(buf, sizeof(buf), "%u", frames);
        lv_label_set_text(s_link_vals[0], buf);
        snprintf(buf, sizeof(buf), "%u", lost);
        lv_label_set_text(s_link_vals[1], buf);
        snprintf(buf, sizeof(buf), "%u", under);
        lv_label_set_text(s_link_vals[2], buf);
    }

    uint64_t sd_total = 0, sd_free = 0;
    switch (phone_storage_get(&sd_total, &sd_free)) {
    case PHONE_STORAGE_MOUNTED:
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB", (sd_total - sd_free) / 1e9, sd_total / 1e9);
        break;
    case PHONE_STORAGE_CHECKING:
        snprintf(buf, sizeof(buf), "%s", T("Đang kiểm tra…", "Checking…"));
        break;
    case PHONE_STORAGE_NO_CARD:
        snprintf(buf, sizeof(buf), "%s", T("Không có thẻ", "No card"));
        break;
    case PHONE_STORAGE_USB:
        snprintf(buf, sizeof(buf), "%s", T("Đang dùng qua USB", "In use over USB"));
        break;
    default:
        snprintf(buf, sizeof(buf), "%s", T("Tắt", "Disabled"));
        break;
    }
    lv_label_set_text(s_sd_val, buf);

    bsp_battery_info_t b;
    if (!phone_power_get_battery(&b)) {
        for (auto *v : s_batt_vals) {
            lv_label_set_text(v, "--");
        }
        return;
    }
    const char *state = T("Không có pin", "No battery");
    if (b.state == BSP_BATTERY_CHARGING) {
        state = T("Đang sạc", "Charging");
    } else if (b.state == BSP_BATTERY_FULL) {
        state = T("Đã sạc đầy", "Fully charged");
    } else if (b.state == BSP_BATTERY_DISCHARGING) {
        state = T("Đang dùng pin", "On battery");
    }
    lv_label_set_text(s_batt_vals[0], state);
    if (b.state == BSP_BATTERY_ABSENT) {
        lv_label_set_text(s_batt_vals[1], "--");
    } else {
        snprintf(buf, sizeof(buf), "%d%%  (%.2f V)", b.percent, b.voltage_mv / 1000.0);
        lv_label_set_text(s_batt_vals[1], buf);
    }
    if (b.vbus_present) {
        snprintf(buf, sizeof(buf), "%.2f V", b.vbus_mv / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "%s", T("Không cắm", "Not connected"));
    }
    lv_label_set_text(s_batt_vals[2], buf);
    snprintf(buf, sizeof(buf), "%.2f V", b.vsys_mv / 1000.0);
    lv_label_set_text(s_batt_vals[3], buf);
    snprintf(buf, sizeof(buf), "%u mA, %.2f V", b.charge_current_ma, b.charge_voltage_mv / 1000.0);
    lv_label_set_text(s_batt_vals[4], buf);
    snprintf(buf, sizeof(buf), "%.1f °C", b.die_temp_c);
    lv_label_set_text(s_batt_vals[5], buf);
}

/* Follow volume changes made with the volume keys while the Sound page is open */
void sync_sound_page()
{
    if (lv_obj_has_state(s_vol_slider, LV_STATE_PRESSED)) {
        return;                 // the user is dragging the slider
    }
    const int level = phone_audio_get_volume();
    const bool muted = level == 0;
    if (muted != s_muted) {
        s_muted = muted;
        if (muted) {
            lv_obj_add_state(s_mute_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_mute_sw, LV_STATE_CHECKED);
        }
    }
    if (lv_slider_get_value(s_vol_slider) != level) {
        lv_slider_set_value(s_vol_slider, level, LV_ANIM_OFF);     /* muted: the lowest */
    }
    if (muted) {
        lv_label_set_text(s_vol_val, T("Tắt tiếng", "Muted"));
    } else {
        lv_label_set_text_fmt(s_vol_val, "%d%%", level);
    }
}

/* Network speed test page (Settings > Debug): Wi-Fi */
lv_obj_t *s_speed_check[2];
lv_obj_t *s_speed_text;
lv_obj_t *s_speed_result[2];        /* last finished download / upload, with its server */
phone_speed_target_t s_speed_target = PHONE_SPEED_LAN;

void speed_update_checks()
{
    for (int i = 0; i < 2; i++) {
        if (s_speed_check[i]) {
            lv_label_set_text(s_speed_check[i], i == s_speed_target ? LV_SYMBOL_OK : "");
        }
    }
}

void speed_refresh()
{
    if (!s_speed_text) {
        return;
    }
    phone_speed_state_t st;
    phone_speedtest_get(&st);
    if (!st.running && !st.failed && st.ms > 0 && s_speed_result[st.upload ? 1 : 0]) {
        char v[96];
        snprintf(v, sizeof(v), "%u.%02u Mbps  (%s)", static_cast<unsigned>(st.kbps / 1000),
                 static_cast<unsigned>(st.kbps % 1000 / 10), st.host);
        lv_obj_t *res = s_speed_result[st.upload ? 1 : 0];
        if (strcmp(lv_label_get_text(res), v) != 0) {
            lv_label_set_text(res, v);
        }
    }
    if (!st.running && !st.failed && st.ms == 0) {
        lv_label_set_text(s_speed_text, T("Chọn máy chủ rồi bấm Tải xuống hoặc Tải lên (10 giây).\n"
                                          "Máy tính: chạy tools/speed_server.py, cùng mạng với board.",
                                          "Pick a server, then Download or Upload (10 s).\n"
                                          "PC: run tools/speed_server.py on the board's network."));
        return;
    }
    const char *dir = st.upload ? T("Tải lên", "Upload") : T("Tải xuống", "Download");
    const char *state = st.running ? T("đang đo", "running") : st.failed ? T("lỗi", "failed") : T("xong", "done");
    lv_label_set_text_fmt(s_speed_text,
                          "%s %s: %s\n"
                          "%u.%02u Mbps\n"
                          "%u.%u MB / %u.%u s\n"
                          "SRAM min %uK   DMA min %uK",
                          dir, st.host, state,
                          static_cast<unsigned>(st.kbps / 1000), static_cast<unsigned>(st.kbps % 1000 / 10),
                          static_cast<unsigned>(st.bytes / 1000000), static_cast<unsigned>(st.bytes % 1000000 / 100000),
                          static_cast<unsigned>(st.ms / 1000), static_cast<unsigned>(st.ms % 1000 / 100),
                          static_cast<unsigned>(st.sram_min == UINT32_MAX ? 0 : st.sram_min / 1024),
                          static_cast<unsigned>(st.dma_min == UINT32_MAX ? 0 : st.dma_min / 1024));
}

void on_speed_target(lv_event_t *e)
{
    s_speed_target = static_cast<phone_speed_target_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    speed_update_checks();
}

void on_speed_start(lv_event_t *e)
{
    const bool upload = lv_event_get_user_data(e) != nullptr;
    phone_speedtest_start(s_speed_target, upload);
    speed_refresh();
}

void build_speed_page(lv_obj_t *root)
{
    lv_obj_t *p = make_page(root, T("Tốc độ mạng", "Network speed"), PAGE_SPEED);
    lv_obj_t *g = make_group(p, T("Máy chủ", "Server"));
    const char *names[2] = {T("Máy tính (speed_server.py)", "PC (speed_server.py)"), "Internet (Cloudflare)"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = grouped_row(g, names[i], on_speed_target, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_speed_check[i] = lv_label_create(row);
        lv_obj_set_style_text_color(s_speed_check[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    group_end(g);
    speed_update_checks();

    g = make_group(p, T("Đo", "Test"));
    grouped_row(g, T(LV_SYMBOL_DOWNLOAD "  Tải xuống", LV_SYMBOL_DOWNLOAD "  Download"), on_speed_start, nullptr);
    grouped_row(g, T(LV_SYMBOL_UPLOAD "  Tải lên", LV_SYMBOL_UPLOAD "  Upload"), on_speed_start, reinterpret_cast<void *>(1));
    group_end(g);

    g = make_group(p, T("Trạng thái", "Status"));
    lv_obj_set_style_pad_all(g, 16, 0);
    s_speed_text = make_text(g, "", COLOR_TEXT);

    g = make_group(p, T("Kết quả", "Results"));
    const char *dirs[2] = {T("Tải xuống", "Download"), T("Tải lên", "Upload")};
    for (int i = 0; i < 2; i++) {
        s_speed_result[i] = kv_row(g, dirs[i]);
        lv_obj_set_style_max_width(s_speed_result[i], 300, 0);
        lv_label_set_long_mode(s_speed_result[i], LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(s_speed_result[i], "--");
    }
    group_end(g);
    speed_refresh();
}

/* Memory speed page (Settings > Debug): PSRAM and SRAM, then the microSD card */
lv_obj_t *s_mem_text;
lv_obj_t *s_sd_text;

void sd_refresh()
{
    if (!s_sd_text) {
        return;
    }
    phone_sdbench_t r;
    phone_sdbench_get(&r);
    if (r.running) {
        lv_label_set_text(s_sd_text, T("Đang đo… (khoảng 10–20 giây)", "Running… (about 10-20 s)"));
    } else if (r.failed) {
        lv_label_set_text_fmt(s_sd_text, "%s: %s", T("Lỗi", "Failed"), r.error);
    } else if (!r.done) {
        lv_label_set_text(s_sd_text, T("Bấm Đo: ghi rồi đọc một tệp 8 MB trên thẻ (xóa sau khi đo), bộ đệm ở SRAM và ở PSRAM, đơn vị KB/s.",
                                       "Tap Run: writes then reads an 8 MB file on the card (deleted after), buffer in SRAM and in PSRAM, in KB/s."));
    } else {
        lv_label_set_text_fmt(s_sd_text, "%s SRAM\n  %s %u   %s %u\n%s PSRAM\n  %s %u   %s %u",
                              T("Bộ đệm", "Buffer in"),
                              T("ghi", "write"), static_cast<unsigned>(r.sram_write),
                              T("đọc", "read"), static_cast<unsigned>(r.sram_read),
                              T("Bộ đệm", "Buffer in"),
                              T("ghi", "write"), static_cast<unsigned>(r.psram_write),
                              T("đọc", "read"), static_cast<unsigned>(r.psram_read));
    }
}

void mem_refresh()
{
    if (!s_mem_text) {
        return;
    }
    phone_membench_t r;
    phone_membench_get(&r);
    if (r.running) {
        lv_label_set_text(s_mem_text, T("Đang đo… (vài giây)", "Running… (a few seconds)"));
    } else if (r.failed) {
        lv_label_set_text(s_mem_text, T("Lỗi: không đủ bộ nhớ để đo", "Failed: not enough memory"));
    } else if (!r.done) {
        lv_label_set_text(s_mem_text, T("Bấm Đo: ghi, đọc và chép trên PSRAM (bộ đệm 1 MB, lớn hơn cache L2) và SRAM, đơn vị MB/s.",
                                        "Tap Run: write, read and copy on PSRAM (1 MB buffers, larger than the L2 cache) and SRAM, in MB/s."));
    } else {
        lv_label_set_text_fmt(s_mem_text,
                              "PSRAM %u MHz\n  %s %u   %s %u   %s %u\n"
                              "SRAM\n  %s %u   %s %u   %s %u",
                              static_cast<unsigned>(r.psram_mhz),
                              T("ghi", "write"), static_cast<unsigned>(r.psram_write),
                              T("đọc", "read"), static_cast<unsigned>(r.psram_read),
                              T("chép", "copy"), static_cast<unsigned>(r.psram_copy),
                              T("ghi", "write"), static_cast<unsigned>(r.sram_write),
                              T("đọc", "read"), static_cast<unsigned>(r.sram_read),
                              T("chép", "copy"), static_cast<unsigned>(r.sram_copy));
    }
}

void build_mem_page(lv_obj_t *root)
{
    lv_obj_t *p = make_page(root, T("Tốc độ bộ nhớ", "Memory speed"), PAGE_MEM);
    lv_obj_t *g = make_card(p);
    grouped_row(g, T(LV_SYMBOL_PLAY "  Đo SRAM/PSRAM", LV_SYMBOL_PLAY "  Run on SRAM/PSRAM"), [](lv_event_t *) {
        phone_membench_start();
        mem_refresh();
    }, nullptr);
    group_end(g);
    g = make_group(p, T("Kết quả (MB/s)", "Result (MB/s)"));
    lv_obj_set_style_pad_all(g, 16, 0);
    s_mem_text = make_text(g, "", COLOR_TEXT);
    mem_refresh();

    g = make_group(p, T("Thẻ nhớ", "microSD card"));
    grouped_row(g, T(LV_SYMBOL_SD_CARD "  Đo thẻ nhớ", LV_SYMBOL_SD_CARD "  Run on the card"), [](lv_event_t *) {
        phone_sdbench_start();
        sd_refresh();
    }, nullptr);
    group_end(g);
    g = make_group(p, T("Kết quả thẻ nhớ (KB/s)", "Card result (KB/s)"));
    lv_obj_set_style_pad_all(g, 16, 0);
    s_sd_text = make_text(g, "", COLOR_TEXT);
    sd_refresh();
}

void on_timer(lv_timer_t *t)
{
    if (s_scan_done.exchange(false)) {
        scan_spin_stop();
        if (s_cur_page == PAGE_WIFI) {
            refresh_wifi_list();
        }
        if (!s_connect_requested && phone_net_is_enabled()) {
            phone_net_enable();     /* the scan paused auto reconnect: resume it (no-op when connected) */
        }
    }
    /* the switch follows quick settings */
    if (s_wifi_sw && lv_obj_has_state(s_wifi_sw, LV_STATE_CHECKED) != phone_net_is_enabled()) {
        s_wifi_on = phone_net_is_enabled();
        if (s_wifi_on) {
            lv_obj_add_state(s_wifi_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_wifi_sw, LV_STATE_CHECKED);
            set_list_empty(true);
        }
        if (s_wifi_body) {
            if (s_wifi_on) {
                lv_obj_remove_flag(s_wifi_body, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_wifi_body, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_cur_page == PAGE_WIFI) {
        refresh_wifi_status();
        refresh_saved();
    } else if (s_cur_page == PAGE_SOUND) {
        sync_sound_page();
    } else if (s_cur_page == PAGE_DISPLAY && !lv_obj_has_state(s_bri_slider, LV_STATE_PRESSED)) {
        /* follow quick settings */
        const int bri = phone_pref_get("bri", 100);
        if (lv_slider_get_value(s_bri_slider) != bri) {
            lv_slider_set_value(s_bri_slider, bri, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_bri_val, "%d%%", bri);
        }
    } else if (s_cur_page == PAGE_SPEED) {
        speed_refresh();
    } else if (s_cur_page == PAGE_MEM) {
        mem_refresh();
        sd_refresh();
    } else if (s_cur_page == PAGE_ABOUT) {
        static int tick;
        if ((++tick % 4) == 0) {
            refresh_about();
        }
    }
}


enum SliderIcons { ICONS_BRIGHTNESS, ICONS_VOLUME };

/* A sun of `side` pixels (the symbol font has none): a disc and eight rays */
lv_obj_t *sun_icon(lv_obj_t *parent, int side)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, side, side);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    const int disc = side * 3 / 7;
    const int ray = std::max(3, side / 7);
    glyph_part(box, (side - disc) / 2, (side - disc) / 2, disc, disc, LV_RADIUS_CIRCLE, 0, true);
    const float r = (side - ray) / 2.0f;
    for (int i = 0; i < 8; i++) {
        const float a = i * 3.14159265f / 4;
        const int x = static_cast<int>(lroundf(r + r * cosf(a)));
        const int y = static_cast<int>(lroundf(r + r * sinf(a)));
        glyph_part(box, x, y, ray, ray, LV_RADIUS_CIRCLE, 0, true);
    }
    return box;
}

lv_obj_t *symbol_icon(lv_obj_t *parent, const char *symbol)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(l, symbol);
    return l;
}

/* A card with the value above a slider, and an icon at each end of the slider (low, high) */
lv_obj_t *build_slider_card(lv_obj_t *root, const char *title, const char *caption, int page, int min, int value,
                            SliderIcons icons, lv_event_cb_t cb, lv_event_cb_t done_cb, lv_obj_t **val_out)
{
    lv_obj_t *p = make_page(root, title, page);
    lv_obj_t *card = make_group(p, caption);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *val = lv_label_create(card);
    lv_label_set_text_fmt(val, "%d%%", value);
    lv_obj_set_style_text_color(val, lv_color_hex(COLOR_TEXT), 0);
    *val_out = val;

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 18, 0);
    /* the knob (pad 9 around the 14 px bar) reaches 9 px above and below the bar: room for it, and no
     * clipping by the row */
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (icons == ICONS_BRIGHTNESS) {
        sun_icon(row, 18);
    } else {
        symbol_icon(row, LV_SYMBOL_MUTE);
    }
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_flex_grow(s, 1);
    lv_obj_set_height(s, 14);
    lv_obj_set_style_pad_all(s, 9, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_slider_set_range(s, min, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s, done_cb, LV_EVENT_RELEASED, nullptr);
    if (icons == ICONS_BRIGHTNESS) {
        sun_icon(row, 28);
    } else {
        symbol_icon(row, LV_SYMBOL_VOLUME_MAX);
    }
    return s;
}

constexpr int TZ_MAX = 24;
lv_obj_t *s_root;
lv_obj_t *s_lang_check[2];
lv_obj_t *s_tz_check[TZ_MAX];

void lang_update_checks()
{
    int cur = static_cast<int>(phone_lang_get());
    for (int i = 0; i < 2; i++) {
        if (s_lang_check[i]) {
            lv_label_set_text(s_lang_check[i], i == cur ? LV_SYMBOL_OK : "");
        }
    }
}

void tz_update_checks()
{
    int cur = phone_tz_get();
    for (int i = 0; i < TZ_MAX; i++) {
        if (s_tz_check[i]) {
            lv_label_set_text(s_tz_check[i], i == cur ? LV_SYMBOL_OK : "");
        }
    }
}

void build_ui();

void rebuild_cb(void *)
{
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = nullptr;
    }
    build_ui();
    show_page(PAGE_LANG);
    phone_power_confirm(PHONE_POWER_RESTART_LANG);
}

void on_lang_pick(lv_event_t *e)
{
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx == static_cast<int>(phone_lang_get())) {
        return;
    }
    phone_lang_set(static_cast<phone_lang_t>(idx));
    lv_async_call(rebuild_cb, nullptr);
}

void on_tz_pick(lv_event_t *e)
{
    phone_tz_set(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    tz_update_checks();
}

void build_ui()
{
    memset(s_pages, 0, sizeof(s_pages));
    memset(s_lang_check, 0, sizeof(s_lang_check));
    memset(s_tz_check, 0, sizeof(s_tz_check));
    memset(s_speed_check, 0, sizeof(s_speed_check));
    s_speed_text = nullptr;
    s_mem_text = nullptr;
    s_sd_text = nullptr;
    s_kb = nullptr;
    s_cur_page = PAGE_MAIN;
    s_wifi_on = phone_net_is_enabled();     /* also switched from quick settings */
    s_saved_sig[0] = '\0';   /* the saved list is rebuilt from scratch with the new widgets */

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *root = lv_obj_create(scr);
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *main_page = make_page(root, T("Cài đặt", "Settings"), PAGE_MAIN);
    lv_obj_t *g = make_group(main_page, T("Kết nối", "Connectivity"));
    group_row(g, LV_SYMBOL_WIFI, 0x2F80ED, "Wi-Fi", nullptr, PAGE_WIFI);
    group_end(g);

    g = make_group(main_page, T("Thiết bị", "Device"));
    group_row(g, LV_SYMBOL_IMAGE, 0xF2994A, T("Màn hình", "Display"), nullptr, PAGE_DISPLAY);
    group_row(g, LV_SYMBOL_AUDIO, 0xEB5757, T("Âm thanh", "Sound"), nullptr, PAGE_SOUND);
    group_end(g);

    g = make_group(main_page, T("Hệ thống", "System"));
    group_row(g, kIconGlobe, 0x9B51E0, T("Ngôn ngữ", "Language"), phone_lang_get() == PHONE_LANG_VI ? "Tiếng Việt" : "English", PAGE_LANG);
    group_row(g, LV_SYMBOL_GPS, 0x2D9CDB, T("Múi giờ", "Time zone"), nullptr, PAGE_TZ);
    group_row(g, LV_SYMBOL_POWER, 0xE5484D, T("Nguồn", "Power"), nullptr, PAGE_POWER);
    group_end(g);

    g = make_group(main_page, T("Thông tin", "Info"));
    group_row(g, kIconInfo, 0x6B7785, T("Giới thiệu", "About"), nullptr, PAGE_ABOUT);
    group_end(g);

    g = make_group(main_page, T("Gỡ lỗi", "Debug"));
    lv_obj_t *mon = switch_row(g, T("Thông số hệ thống", "System monitor"), phone_monitor_is_on(), [](lv_event_t *e) {
        phone_monitor_set(lv_obj_has_state(static_cast<lv_obj_t *>(lv_event_get_target(e)), LV_STATE_CHECKED));
    });
    add_tile(lv_obj_get_parent(mon), kIconChart, 0x27AE60);
    group_row(g, LV_SYMBOL_DOWNLOAD, 0x2F80ED, T("Tốc độ mạng", "Network speed"), nullptr, PAGE_SPEED);
    group_row(g, LV_SYMBOL_CHARGE, 0xF2C94C, T("Tốc độ bộ nhớ", "Memory speed"), nullptr, PAGE_MEM);
    group_end(g);

    lv_obj_t *wifi = make_page(root, "Wi-Fi", PAGE_WIFI);
    lv_obj_t *sw_card = make_card(wifi);
    s_wifi_sw = switch_row(sw_card, "Wi-Fi", s_wifi_on, on_wifi_toggle);
    group_end(sw_card);

    s_wifi_body = lv_obj_create(wifi);
    lv_obj_remove_style_all(s_wifi_body);
    lv_obj_set_width(s_wifi_body, LV_PCT(100));
    lv_obj_set_height(s_wifi_body, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_wifi_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_body, 12, 0);
    lv_obj_remove_flag(s_wifi_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *st = make_group(s_wifi_body, T("Trạng thái", "Status"));
    lv_obj_t *st_row = lv_obj_create(st);
    lv_obj_remove_style_all(st_row);
    lv_obj_set_width(st_row, LV_PCT(100));
    lv_obj_set_height(st_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(st_row, 16, 0);
    lv_obj_remove_flag(st_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *info = lv_obj_create(st_row);
    lv_obj_remove_style_all(info);
    lv_obj_set_width(info, LV_PCT(100));
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_status = lv_label_create(info);
    lv_obj_set_width(s_wifi_status, LV_PCT(100));
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COLOR_SUB), 0);
    group_end(st);

    s_saved_caption = make_caption(s_wifi_body, T("Mạng đã lưu", "Saved networks"));
    s_saved_card = make_card(s_wifi_body);
    s_saved_list = lv_obj_create(s_saved_card);
    lv_obj_remove_style_all(s_saved_list);
    lv_obj_set_width(s_saved_list, LV_PCT(100));
    lv_obj_set_height(s_saved_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_saved_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_saved_caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_saved_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *head = lv_obj_create(s_wifi_body);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_list_cap = make_caption(head, T("Mạng khả dụng", "Available networks"));
    lv_obj_t *scan_btn = lv_obj_create(head);
    lv_obj_remove_style_all(scan_btn);
    lv_obj_set_size(scan_btn, 64, 56);
    lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(scan_btn, 28, 0);
    lv_obj_set_style_bg_opa(scan_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(COLOR_ROW), LV_STATE_PRESSED);
    lv_obj_add_event_cb(scan_btn, on_scan, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ic = lv_label_create(scan_btn);
    lv_label_set_text(ic, LV_SYMBOL_REFRESH);
    s_scan_icon = ic;
    lv_obj_set_style_text_color(ic, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_center(ic);

    s_wifi_list = make_card(s_wifi_body);
    lv_obj_set_style_radius(s_wifi_list, 8, 0);
    set_list_empty(true);
    if (!s_wifi_on) {
        lv_obj_add_flag(s_wifi_body, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *pass = make_page(root, T("Mật khẩu", "Password"), PAGE_PASS);
    s_pass_title = make_text(pass, "", COLOR_SUB);
    lv_obj_t *pass_row = lv_obj_create(pass);
    lv_obj_remove_style_all(pass_row);
    lv_obj_set_width(pass_row, LV_PCT(100));
    lv_obj_set_height(pass_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pass_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pass_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pass_row, 12, 0);
    lv_obj_remove_flag(pass_row, LV_OBJ_FLAG_SCROLLABLE);
    s_pass_ta = lv_textarea_create(pass_row);
    lv_obj_set_flex_grow(s_pass_ta, 1);
    lv_obj_t *eye = lv_obj_create(pass_row);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, 64, 56);
    lv_obj_add_flag(eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(eye, 28, 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COLOR_ROW), LV_STATE_PRESSED);
    lv_obj_add_event_cb(eye, on_eye_toggle, LV_EVENT_CLICKED, nullptr);
    s_eye_lbl = lv_label_create(eye);
    lv_obj_set_style_text_color(s_eye_lbl, lv_color_hex(COLOR_TEXT), 0);
    lv_label_set_text(s_eye_lbl, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(s_eye_lbl);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, T("Nhập mật khẩu", "Enter password"));
    lv_obj_t *kb = lv_keyboard_create(root);
    lv_obj_set_size(kb, LV_PCT(100), 280);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -NAV_STRIP_H);    /* above the touch guard */
    lv_keyboard_set_textarea(kb, s_pass_ta);
    lv_obj_add_event_cb(kb, on_pass_ready, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, on_kb_cancel, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(s_pass_ta, on_pass_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    s_kb = kb;

    s_bri_slider = build_slider_card(root, T("Màn hình", "Display"), T("Độ sáng", "Brightness"), PAGE_DISPLAY, 5,
                      phone_pref_get("bri", 100), ICONS_BRIGHTNESS, on_brightness, on_brightness_done, &s_bri_val);
    lv_obj_t *to = make_group(s_pages[PAGE_DISPLAY], T("Tự tắt màn hình", "Screen timeout"));
    for (int i = 0; i < phone_lock_timeout_count(); i++) {
        lv_obj_t *row = grouped_row(to, phone_lock_timeout_label(i), on_timeout_pick, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_to_check[i] = lv_label_create(row);
        lv_obj_set_style_text_color(s_to_check[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    group_end(to);
    to_update_checks();

    s_muted = phone_pref_get("mute", 0) != 0;
    s_vol_slider = build_slider_card(root, T("Âm thanh", "Sound"), T("Âm lượng", "Volume"), PAGE_SOUND, 0,
                                     phone_pref_get("vol", 60), ICONS_VOLUME, on_volume, on_volume_done, &s_vol_val);
    lv_obj_t *sound_opts = make_group(s_pages[PAGE_SOUND], T("Tùy chọn", "Options"));
    s_mute_sw = switch_row(sound_opts, T("Tắt tiếng", "Mute"), s_muted, on_mute_toggle);
    group_end(sound_opts);
    if (s_muted) {
        lv_slider_set_value(s_vol_slider, 0, LV_ANIM_OFF);
        lv_label_set_text(s_vol_val, T("Tắt tiếng", "Muted"));
    }
    lv_obj_t *lang = make_page(root, T("Ngôn ngữ", "Language"), PAGE_LANG);
    const char *lang_names[2] = {"Tiếng Việt", "English"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = make_row(lang, lang_names[i], "", on_lang_pick, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_lang_check[i] = lv_obj_get_child(row, 1);
    }
    lang_update_checks();

    lv_obj_t *tz = make_page(root, T("Múi giờ", "Time zone"), PAGE_TZ);
    for (int i = 0; i < phone_tz_count() && i < TZ_MAX; i++) {
        lv_obj_t *row = make_row(tz, phone_tz_label(i), "", on_tz_pick, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        s_tz_check[i] = lv_obj_get_child(row, 1);
    }
    tz_update_checks();

    lv_obj_t *power = make_page(root, T("Nguồn", "Power"), PAGE_POWER);
    make_row(power, T("Tắt nguồn", "Power off"), nullptr, on_power_pick, reinterpret_cast<void *>(PHONE_POWER_OFF));
    make_row(power, T("Khởi động lại", "Restart"), nullptr, on_power_pick, reinterpret_cast<void *>(PHONE_POWER_RESTART));

    build_speed_page(root);
    build_mem_page(root);

    lv_obj_t *about = make_page(root, T("Giới thiệu", "About"), PAGE_ABOUT);
    make_text(about, "KSDIY ESP32-P4C5", COLOR_TEXT);
    make_text(about, "ESP32-P4 + ESP32-S3 (StickS3) · 480×800", COLOR_SUB);
    lv_obj_t *dev = make_group(about, T("Thiết bị", "Device"));
    const char *keys[5] = {"Chip", "RAM", "PSRAM", "Flash", "MAC"};
    for (int i = 0; i < 5; i++) {
        s_about_vals[i] = kv_row(dev, keys[i]);
    }
    group_end(dev);

    lv_obj_t *sw = make_group(about, T("Phần mềm", "Software"));
    const esp_app_desc_t *app = esp_app_get_description();
    char ver[40];
    sw_row(sw, T("Phiên bản", "Version"), app->version);
    snprintf(ver, sizeof(ver), "%s %.5s", app->date, app->time);
    sw_row(sw, T("Ngày build", "Built"), ver);
    sw_row(sw, "ESP-IDF", esp_get_idf_version());
    s_c5_ver_val = kv_row(sw, T("Firmware Wi-Fi (S3)", "Wi-Fi firmware (S3)"));
    snprintf(ver, sizeof(ver), "%d.%d.%d", ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    sw_row(sw, "ESP-Hosted", ver);
    snprintf(ver, sizeof(ver), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    sw_row(sw, "LVGL", ver);
    snprintf(ver, sizeof(ver), "%d.%d.%d", BROOKESIA_CORE_VER_MAJOR, BROOKESIA_CORE_VER_MINOR, BROOKESIA_CORE_VER_PATCH);
    sw_row(sw, "Brookesia", ver);
    group_end(sw);

    lv_obj_t *lk = make_group(about, T("Liên kết S3 (StickS3)", "S3 link (StickS3)"));
    char lkv[40];
    snprintf(lkv, sizeof(lkv), "SPI %d MHz", CONFIG_ESP_HOSTED_HOST_SPI_CLK_MHZ);
    sw_row(lk, T("Wi-Fi (ESP-Hosted)", "Wi-Fi (ESP-Hosted)"), lkv);
    sw_row(lk, T("Âm thanh (I2S)", "Audio (I2S)"), "16 kHz mono");
    s_link_vals[0] = kv_row(lk, T("Khung I2S nhận", "I2S frames"));
    s_link_vals[1] = kv_row(lk, T("Khung lỗi / mất", "Lost frames"));
    s_link_vals[2] = kv_row(lk, T("Underrun loa", "Speaker underruns"));
    group_end(lk);

    /* Checked once when the app opens: an I2C address probe per chip */
    lv_obj_t *hw = make_group(about, T("Phần cứng", "Hardware"));
    hw_row(hw, T("Màn hình", "Display"), "ST7102 480×800", 0);
    hw_row(hw, T("Cảm ứng", "Touch"), "ST7123", 0x55);
    hw_row(hw, "PMIC", "AXP2101", 0x34);
    hw_row(hw, T("Loa", "Speaker"), "StickS3 (I2S)", 0);
    hw_row(hw, "Mic", "StickS3 + AEC", 0);
    hw_row(hw, "IMU", "LSM6DS3TR-C", 0x6A);
    hw_row(hw, "Camera", "SC2336 (CSI)", 0);
    s_sd_val = kv_row(hw, T("Thẻ nhớ", "microSD"));
    group_end(hw);
    lv_obj_t *batt = make_group(about, T("Pin và nguồn", "Battery and power"));
    const char *bkeys[6] = {T("Trạng thái", "Status"), T("Mức pin", "Level"), "USB", T("Điện áp hệ thống", "System voltage"),
                            T("Bộ sạc", "Charger"), T("Nhiệt độ PMIC", "PMIC temperature")};
    for (int i = 0; i < 6; i++) {
        s_batt_vals[i] = kv_row(batt, bkeys[i]);
    }
    group_end(batt);
    refresh_about();

    /* A touch that starts in the navigation strip must not scroll the page under it. The system
     * reads that gesture straight from the touch panel. */
    lv_obj_t *guard = lv_obj_create(root);
    lv_obj_remove_style_all(guard);
    lv_obj_set_size(guard, LV_PCT(100), NAV_STRIP_H);
    lv_obj_align(guard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(guard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(guard, LV_OBJ_FLAG_SCROLLABLE);

    show_page(PAGE_MAIN);
}

} // namespace

namespace esp_brookesia::apps {

PhoneSettings *PhoneSettings::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneSettings();
    }
    return _instance;
}

PhoneSettings::PhoneSettings():
    App(phone_tr("Cài đặt", "Settings"), &phone_settings_icon, true, true, true)
{
}

PhoneSettings::~PhoneSettings()
{
}

bool PhoneSettings::run()
{
    build_ui();
    lv_timer_create(on_timer, 500, nullptr);
    if (s_open_wifi) {
        s_open_wifi = false;
        show_page(PAGE_WIFI);
    }
    return true;
}

bool PhoneSettings::resume()
{
    if (s_open_wifi) {
        s_open_wifi = false;
        show_page(PAGE_WIFI);
    }
    return true;
}

bool PhoneSettings::back()
{
    if (s_cur_page == PAGE_PASS) {
        show_page(PAGE_WIFI);
    } else if (s_cur_page != PAGE_MAIN) {
        show_page(PAGE_MAIN);
    } else {
        return notifyCoreClosed();
    }
    return true;
}

} // namespace esp_brookesia::apps

void phone_settings_open_wifi_next(void)
{
    s_open_wifi = true;
}

namespace esp_brookesia::apps {

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneSettings, APP_NAME, []()
{
    return std::shared_ptr<PhoneSettings>(PhoneSettings::requestInstance(), [](PhoneSettings * p) {});
})

} // namespace esp_brookesia::apps
