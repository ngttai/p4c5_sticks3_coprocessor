/*
 * Radio app: station name, status and stream format, a 12-band spectrum, previous / play-stop / next,
 * volume, and the station list (from appdata/radio/stations.json on the card, see radio_stations.cpp). The sound and
 * the spectrum come from radio_player.cpp; this file only reads their state from an LVGL timer.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "esp_log.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "esp_lib_utils.h"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_storage.h"
#include "phone_quick.h"
#include "radio_player.hpp"
#include "phone_spectrum.hpp"
#include "radio_stations.hpp"
#include "phone_app_radio.hpp"

#define APP_NAME "Radio"
#define T(vi, en) phone_tr(vi, en)

extern "C" const lv_image_dsc_t phone_radio_icon;

namespace esp_brookesia::apps {

namespace {

const char *TAG = "phone_radio";
const char *HOLD_NAME = "Radio";            // the station file is read from the card
const char *PREF_LAST = "rad_last";

constexpr uint32_t COLOR_BG = 0x0E1116;
constexpr uint32_t COLOR_CARD = 0x171C24;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_ROW_CUR = 0x1F3A3A;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x2EC4B6;
constexpr uint32_t COLOR_BUTTON = 0x2A333C;
constexpr int VIZ_H = 240;
constexpr int BAR_GAP = 10;
constexpr int BAR_MIN = 8;
constexpr int NAV_STRIP = 36;               // swipe-up strip of the navigation bar at the bottom

std::vector<radio::Station> s_stations;
radio::ListResult s_list_result;
int s_cur;
bool s_playing;                             // the user started the sound

lv_obj_t *s_root;
lv_obj_t *s_title;
lv_obj_t *s_status;
lv_obj_t *s_format;
lv_obj_t *s_viz;
spectrum::View s_view;                      // bars of the shared spectrum
lv_obj_t *s_play_label;
lv_obj_t *s_vol_slider;
lv_obj_t *s_vol_label;
lv_obj_t *s_source;
lv_obj_t *s_player_box;                     // controls, hidden when there is no station
lv_obj_t *s_empty_box;                      // how to add the station file
lv_obj_t *s_empty_text;
lv_obj_t *s_overlay;                        // station list
lv_obj_t *s_list;
lv_timer_t *s_timer;
int s_tick;
int s_shown_status = -1;
const char *s_status_text = "";
int s_shown_cur = -2;

void play_current()
{
    if (s_cur < 0 || s_cur >= static_cast<int>(s_stations.size())) {
        return;
    }
    s_playing = true;
    radio::play(s_stations[s_cur].url);
    phone_pref_set(PREF_LAST, s_cur);
}

void select_station(int idx)
{
    const int n = static_cast<int>(s_stations.size());
    if (n == 0) {
        return;
    }
    s_cur = (idx % n + n) % n;
    play_current();
}

void on_play(lv_event_t *)
{
    if (s_playing) {
        s_playing = false;
        radio::stop();
    } else {
        play_current();
    }
}

void on_prev(lv_event_t *)
{
    select_station(s_cur - 1);
}

void on_next(lv_event_t *)
{
    select_station(s_cur + 1);
}

void show_volume()
{
    lv_label_set_text_fmt(s_vol_label, "%d%%", phone_audio_get_volume());
}

void on_volume(lv_event_t *e)
{
    phone_audio_set_volume(lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(e))));
    show_volume();
}

/* ---- station list ---- */

void close_list()
{
    if (s_overlay) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void mark_rows()
{
    if (!s_list) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        const bool cur = static_cast<int>(i) == s_cur;
        lv_obj_set_style_bg_color(row, lv_color_hex(cur ? COLOR_ROW_CUR : COLOR_ROW), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(row, 0), lv_color_hex(cur ? COLOR_ACCENT : 0xFFFFFF), 0);
    }
}

void on_row(lv_event_t *e)
{
    select_station(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    mark_rows();
    close_list();
}

void open_list(lv_event_t *)
{
    if (!s_overlay) {
        return;
    }
    mark_rows();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    if (s_cur >= 0 && s_cur < static_cast<int>(lv_obj_get_child_count(s_list))) {
        lv_obj_scroll_to_view(lv_obj_get_child(s_list, s_cur), LV_ANIM_OFF);
    }
}

void build_list()
{
    lv_obj_clean(s_list);
    for (size_t i = 0; i < s_stations.size(); i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 60);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ACCENT), LV_STATE_PRESSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_text_fmt(name, "%u.  %s", static_cast<unsigned>(i + 1), s_stations[i].name.c_str());
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

/* ---- loading the list ---- */

void show_list_state()
{
    const bool ok = s_list_result == radio::ListResult::OK;
    if (ok) {
        lv_obj_add_flag(s_empty_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_player_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_source, T("Thẻ nhớ · %u đài", "microSD · %u stations"),
                              static_cast<unsigned>(s_stations.size()));
        return;
    }
    lv_obj_add_flag(s_player_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_empty_box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title, T("Chưa có danh sách đài", "No station list"));
    lv_label_set_text(s_format, "");
    lv_label_set_text(s_source, "");
    const char *why = "";
    switch (s_list_result) {
    case radio::ListResult::NO_CARD:
        why = T("Không đọc được thẻ nhớ (chưa cắm, hoặc đang làm ổ USB).",
                "The microSD card cannot be read (absent, or lent as a USB drive).");
        break;
    case radio::ListResult::NO_FILE:
        why = T("Thẻ nhớ chưa có tệp appdata/radio/stations.json.", "The card has no appdata/radio/stations.json.");
        break;
    case radio::ListResult::BAD_FILE:
        why = T("Tệp appdata/radio/stations.json bị lỗi (không phải JSON, hoặc lớn hơn 64 KB).",
                "appdata/radio/stations.json is broken (not JSON, or larger than 64 KB).");
        break;
    default:
        why = T("Tệp appdata/radio/stations.json không có đài nào dùng được.", "appdata/radio/stations.json has no usable station.");
        break;
    }
    lv_label_set_text(s_status, why);
    lv_label_set_text(s_empty_text,
                      T("Chép tệp mẫu sdcard/appdata/radio/stations.json trong repo vào thư mục appdata/radio của thẻ "
                        "(dùng ổ USB của app Tệp), rồi bấm Thử lại.\n\nMỗi đài: { \"name\": \"Tên đài\", \"url\": \"https://.../playlist.m3u8\" }",
                        "Copy the sample sdcard/appdata/radio/stations.json of the repository to the appdata/radio folder of "
                        "the card (USB drive of the Files app), then tap Retry.\n\nEach station: { \"name\": \"Name\", \"url\": \"https://.../playlist.m3u8\" }"));
}

void load_list(bool autoplay)
{
    s_list_result = radio::load_stations(s_stations);
    const int n = static_cast<int>(s_stations.size());
    const int last = phone_pref_get(PREF_LAST, 0);
    s_cur = n ? (last >= 0 && last < n ? last : 0) : -1;
    s_shown_cur = -2;
    s_shown_status = -1;
    build_list();
    show_list_state();
    if (autoplay && n) {
        play_current();
    }
}

void on_retry(lv_event_t *)
{
    load_list(true);
}

/* ---- refresh ---- */

void update_bars()
{
    /* not under the quick settings panel: it covers the bars, and each change would redraw it */
    if (!phone_quick_visible()) {
        /* the levels are shared: another player (music in the background) feeds them too */
        static bool cleared;
        if (radio::status() == radio::Status::PLAYING) {
            spectrum::view_update(s_view);
            cleared = false;
        } else if (!cleared) {
            spectrum::view_clear(s_view);           /* once: it restyles every bar */
            cleared = true;
        }
    }
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    update_bars();
    if (++s_tick % 6 != 0) {                /* labels about 4 times a second */
        return;
    }
    if (s_list_result != radio::ListResult::OK) {
        return;
    }
    if (s_cur != s_shown_cur) {
        s_shown_cur = s_cur;
        lv_label_set_text(s_title, s_stations[s_cur].name.c_str());
    }
    const radio::Status st = radio::status();
    if (static_cast<int>(st) != s_shown_status) {
        s_shown_status = static_cast<int>(st);
        const char *text = "";
        switch (st) {
        case radio::Status::STOPPED:
            text = T("Đã dừng", "Stopped");
            break;
        case radio::Status::CONNECTING:
            text = T("Đang kết nối…", "Connecting…");
            break;
        case radio::Status::PLAYING:
            text = T("Đang phát", "Playing");
            break;
        case radio::Status::RETRYING:
            text = T("Mất kết nối, đang thử lại…", "Connection lost, retrying…");
            break;
        case radio::Status::FAILED:
            text = radio::busy_speaker() ? T("Loa đang bận (app khác đang phát)", "The speaker is busy (another app plays)")
                   : T("Không phát được đài này", "Cannot play this station");
            break;
        }
        lv_obj_set_style_text_color(s_status, lv_color_hex(st == radio::Status::PLAYING ? COLOR_ACCENT : COLOR_SUB), 0);
        if (st == radio::Status::FAILED || st == radio::Status::STOPPED) {
            s_playing = false;
        }
        lv_label_set_text(s_play_label, s_playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
        s_status_text = text;
    }
    /* while playing, the song of an Icecast stream (ICY title) in place of "Playing" */
    char song[160] = "";
    if (st == radio::Status::PLAYING) {
        radio::title(song, sizeof(song));
    }
    const char *want = song[0] ? song : s_status_text;
    if (strcmp(lv_label_get_text(s_status), want) != 0) {
        lv_label_set_long_mode(s_status, song[0] ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(s_status, want);
    }
    char fmt[48];
    radio::format(fmt, sizeof(fmt));
    if (strcmp(lv_label_get_text(s_format), fmt) != 0) {
        lv_label_set_text(s_format, fmt);
    }
    if (!lv_obj_has_state(s_vol_slider, LV_STATE_PRESSED) && lv_slider_get_value(s_vol_slider) != phone_audio_get_volume()) {
        lv_slider_set_value(s_vol_slider, phone_audio_get_volume(), LV_ANIM_OFF);
        show_volume();
    }
}

/* ---- layout ---- */

lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int size, uint32_t color, lv_event_cb_t cb,
                       lv_obj_t **label = nullptr)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    if (label) {
        *label = l;
    }
    return b;
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *centered_label(lv_obj_t *parent, int width, int size, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, width);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, phone_font_by_size(size), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

void build_ui()
{
    s_tick = 0;
    lv_obj_t *root = plain(lv_screen_active());
    s_root = root;
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
        s_overlay = nullptr;
        s_list = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    const int w = lv_obj_get_width(root);
    const int h = lv_obj_get_height(root);

    s_title = centered_label(root, w - 32, 28, 0xFFFFFF);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 20);
    s_status = centered_label(root, w - 48, 18, COLOR_SUB);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 66);
    s_format = centered_label(root, w - 48, 16, COLOR_SUB);
    lv_obj_align(s_format, LV_ALIGN_TOP_MID, 0, 96);

    /* spectrum */
    s_viz = plain(root);
    lv_obj_set_size(s_viz, w - 32, VIZ_H);
    lv_obj_align(s_viz, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_opa(s_viz, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_viz, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(s_viz, 18, 0);
    lv_obj_set_style_pad_all(s_viz, 18, 0);
    spectrum::view_create(s_view, s_viz, BAR_GAP, BAR_MIN, 4);

    /* controls, volume, list */
    s_player_box = plain(root);
    lv_obj_set_size(s_player_box, w, h - 132 - VIZ_H);
    lv_obj_set_y(s_player_box, 132 + VIZ_H);
    lv_obj_align(round_button(s_player_box, LV_SYMBOL_PREV, 64, COLOR_BUTTON, on_prev), LV_ALIGN_TOP_MID, -110, 36);
    lv_obj_align(round_button(s_player_box, LV_SYMBOL_PLAY, 88, COLOR_ACCENT, on_play, &s_play_label), LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_align(round_button(s_player_box, LV_SYMBOL_NEXT, 64, COLOR_BUTTON, on_next), LV_ALIGN_TOP_MID, 110, 36);

    lv_obj_t *vol_icon = lv_label_create(s_player_box);
    lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(vol_icon, lv_color_hex(COLOR_SUB), 0);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(vol_icon, LV_ALIGN_TOP_LEFT, 28, 150);
    s_vol_slider = lv_slider_create(s_player_box);
    lv_obj_set_size(s_vol_slider, w - 170, 10);
    lv_obj_align(s_vol_slider, LV_ALIGN_TOP_LEFT, 74, 158);
    lv_obj_set_ext_click_area(s_vol_slider, 18);
    lv_obj_set_style_pad_all(s_vol_slider, 8, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(COLOR_ACCENT), LV_PART_KNOB);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, phone_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_vol_slider, on_volume, LV_EVENT_VALUE_CHANGED, nullptr);
    s_vol_label = lv_label_create(s_player_box);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_vol_label, phone_font_by_size(16), 0);
    lv_obj_align(s_vol_label, LV_ALIGN_TOP_RIGHT, -24, 150);
    show_volume();

    lv_obj_t *list_btn = lv_button_create(s_player_box);
    lv_obj_remove_style_all(list_btn);
    lv_obj_set_size(list_btn, w - 64, 56);
    lv_obj_align(list_btn, LV_ALIGN_TOP_MID, 0, 204);
    lv_obj_set_style_radius(list_btn, 14, 0);
    lv_obj_set_style_bg_opa(list_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(list_btn, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_color(list_btn, lv_color_hex(COLOR_BUTTON), LV_STATE_PRESSED);
    lv_obj_add_event_cb(list_btn, open_list, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *list_lbl = lv_label_create(list_btn);
    lv_label_set_text_fmt(list_lbl, LV_SYMBOL_LIST "  %s", T("Danh sách đài", "Stations"));
    lv_obj_center(list_lbl);
    s_source = centered_label(s_player_box, w - 48, 14, COLOR_SUB);
    lv_obj_align(s_source, LV_ALIGN_TOP_MID, 0, 270);

    /* no station: how to add the file */
    s_empty_box = plain(root);
    lv_obj_set_size(s_empty_box, w, h - 132 - VIZ_H);
    lv_obj_set_y(s_empty_box, 132 + VIZ_H);
    s_empty_text = lv_label_create(s_empty_box);
    lv_obj_set_width(s_empty_text, w - 48);
    lv_obj_set_style_text_font(s_empty_text, phone_font_by_size(16), 0);
    lv_obj_set_style_text_color(s_empty_text, lv_color_hex(COLOR_SUB), 0);
    lv_label_set_long_mode(s_empty_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_empty_text, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *retry = lv_button_create(s_empty_box);
    lv_obj_remove_style_all(retry);
    lv_obj_set_size(retry, 200, 56);
    lv_obj_align(retry, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_set_style_radius(retry, 28, 0);
    lv_obj_set_style_bg_opa(retry, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(retry, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(retry, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(retry, on_retry, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *retry_lbl = lv_label_create(retry);
    lv_label_set_text_fmt(retry_lbl, LV_SYMBOL_REFRESH "  %s", T("Thử lại", "Retry"));
    lv_obj_center(retry_lbl);

    /* station list over the player */
    s_overlay = plain(root);
    lv_obj_set_size(s_overlay, w, h);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *head = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(head, phone_font_by_size(24), 0);
    lv_label_set_text(head, T("Danh sách đài", "Stations"));
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_t *close_btn = round_button(s_overlay, LV_SYMBOL_CLOSE, 48, COLOR_BUTTON, [](lv_event_t *) {
        close_list();
    });
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -16, 12);
    s_list = plain(s_overlay);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_list, w, h - 76 - NAV_STRIP);
    lv_obj_set_y(s_list, 76);
    lv_obj_set_style_pad_hor(s_list, 12, 0);
    lv_obj_set_style_pad_bottom(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(s_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, lv_color_white(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_40, LV_PART_SCROLLBAR);

    s_timer = lv_timer_create(on_timer, 40, nullptr);
}

} // namespace

PhoneRadio *PhoneRadio::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneRadio();
    }
    return _instance;
}

PhoneRadio::PhoneRadio():
    App(phone_tr("Radio", "Radio"), &phone_radio_icon, true, true, true)
{
}

PhoneRadio::~PhoneRadio()
{
}

bool PhoneRadio::run()
{
    phone_storage_hold(HOLD_NAME);
    build_ui();
    load_list(true);
    ESP_LOGI(TAG, "open");
    return true;
}

/* Back closes the station list first, then the app */
bool PhoneRadio::back()
{
    if (s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        close_list();
        return true;
    }
    return notifyCoreClosed();
}

bool PhoneRadio::close()
{
    s_timer = nullptr;                      /* the core deletes the timers of the app with its screen */
    s_playing = false;
    radio::shutdown();
    phone_storage_release(HOLD_NAME);
    return true;
}

/* Home: the radio keeps playing in the background (and with the screen off); closing the app stops
 * it, and so does another player taking the speaker */
bool PhoneRadio::keepRunning() const
{
    const radio::Status st = radio::status();
    return st == radio::Status::PLAYING || st == radio::Status::CONNECTING || st == radio::Status::RETRYING;
}

bool PhoneRadio::pause()
{
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneRadio, APP_NAME, []()
{
    return std::shared_ptr<PhoneRadio>(PhoneRadio::requestInstance(), [](PhoneRadio * p) {});
})

} // namespace esp_brookesia::apps
