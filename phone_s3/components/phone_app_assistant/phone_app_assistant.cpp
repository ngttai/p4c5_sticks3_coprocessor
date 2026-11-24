/*
 * AI Assistant app: voice chat with the xiaozhi.me service, through the esp_xiaozhi component.
 *
 * Setup: a task asks the server for the device configuration (esp_xiaozhi_chat_get_info, HTTPS).
 * When the device is not bound to an account yet, the server answers with a 6-digit code: the
 * code is shown, and the request is repeated until the user has entered it on the site.
 *
 * Chat: once activated, the app connects (assistant_chat) and shows an animated face
 * (assistant_emote), the conversation as bubbles, and one microphone button:
 *   idle -> tap: the assistant listens; it answers, then listens again by itself;
 *   listening or thinking -> tap: end the conversation;
 *   speaking -> tap: cut the answer and listen.
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_xiaozhi_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "ASSIST"
#include "esp_lib_utils.h"
#include "phone_ui.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_lock.h"
#include "phone_net.h"
#include "assistant_chat.h"
#include "assistant_emote.h"
#include "phone_app_assistant.hpp"

#define APP_NAME "Trợ lý AI"

extern "C" const lv_image_dsc_t phone_assistant_icon;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_assistant";

constexpr uint32_t ACTIVATION_POLL_MS = 10000;  // between two checks while waiting for the code to be entered
constexpr uint32_t RETRY_MS = 3000;             // after a failed request
constexpr int MAX_FAILURES = 5;                 // failed requests in a row before giving up (none once a code is shown)
constexpr int MAX_BUBBLES = 30;
constexpr int NAV_STRIP_H = 36;                 // bottom strip where the swipe-up for the navigation bar starts
constexpr int BUTTON_D = 76;
constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_CARD = 0x1E252C;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0x6C7CFF;
constexpr uint32_t COLOR_OK = 0x22C55E;
constexpr uint32_t COLOR_ERROR = 0xF87171;
constexpr uint32_t COLOR_LISTEN = 0xEF4444;

enum State { ST_IDLE, ST_NO_WIFI, ST_FETCHING, ST_ACTIVATE, ST_READY, ST_ERROR };

lv_obj_t *s_root;
lv_obj_t *s_setup;          // connection and activation page
lv_obj_t *s_status;
lv_obj_t *s_code_card;
lv_obj_t *s_code;
lv_obj_t *s_hint;
lv_obj_t *s_retry;
lv_obj_t *s_chat;           // conversation page
lv_obj_t *s_chat_status;
lv_obj_t *s_bubbles;
lv_obj_t *s_mic;
lv_obj_t *s_mic_label;
lv_obj_t *s_mic_icon;        // drawn microphone, shown when a tap starts a conversation
lv_obj_t *s_mic_ring;
int s_shown_state = -1;
int s_shown_chat = -1;
int s_bubble_count;
char s_shown_code[16];
bool s_chat_started;

std::atomic<int> s_state{ST_IDLE};
std::atomic<uint32_t> s_generation;         // a newer run of the task makes the older one stop
std::atomic<bool> s_task_running;
std::atomic<bool> s_has_mqtt, s_has_websocket;
char s_code_text[16];                       // written by the task before it sets ST_ACTIVATE

/* ---- setup: configuration and activation ---- */

/* Ask the server until the device is activated; ends when ready, on error, or when superseded */
void info_task(void *arg)
{
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    auto current = [generation]() {
        return s_generation == generation;
    };
    int failures = 0;
    bool have_code = false;
    while (current()) {
        if (!phone_net_online()) {
            if (!have_code) {
                s_state = ST_NO_WIFI;       /* with a code on the screen, keep it: the link usually comes back */
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!have_code) {
            s_state = ST_FETCHING;
        }
        esp_xiaozhi_chat_info_t info = {};
        const esp_err_t err = esp_xiaozhi_chat_get_info(&info);
        if (!current()) {
            esp_xiaozhi_chat_free_info(&info);
            break;
        }
        if (err != ESP_OK) {
            /* a dropped connection is common on Wi-Fi: try again, and never drop a code already shown */
            ESP_LOGW(TAG, "get_info: %s (%d in a row)", esp_err_to_name(err), failures + 1);
            esp_xiaozhi_chat_free_info(&info);
            if (!have_code && ++failures >= MAX_FAILURES) {
                s_state = ST_ERROR;
                break;
            }
            for (uint32_t waited = 0; waited < RETRY_MS && current(); waited += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        failures = 0;
        if (info.has_activation_code || info.has_activation_challenge) {
            strlcpy(s_code_text, info.activation_code ? info.activation_code : "", sizeof(s_code_text));
            ESP_LOGI(TAG, "not activated yet, code received");
            have_code = true;
            s_state = ST_ACTIVATE;
            esp_xiaozhi_chat_free_info(&info);
            for (uint32_t waited = 0; waited < ACTIVATION_POLL_MS && current(); waited += 100) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        s_has_mqtt = info.has_mqtt_config;
        s_has_websocket = info.has_websocket_config;
        ESP_LOGI(TAG, "activated: mqtt %d, websocket %d", info.has_mqtt_config, info.has_websocket_config);
        esp_xiaozhi_chat_free_info(&info);
        s_state = ST_READY;
        break;
    }
    if (current()) {
        s_task_running = false;
    }
    vTaskDelete(nullptr);
}

void start_info_task()
{
    const uint32_t generation = ++s_generation;
    s_state = ST_FETCHING;
    s_task_running = true;
    if (xTaskCreate(info_task, "assistant_info", 8192, reinterpret_cast<void *>(static_cast<uintptr_t>(generation)), 4,
                    nullptr) != pdPASS) {
        s_task_running = false;
        s_state = ST_ERROR;
    }
}

void on_retry(lv_event_t *)
{
    start_info_task();
}

void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

void show_setup_state(int state)
{
    lv_obj_set_flag(s_code_card, LV_OBJ_FLAG_HIDDEN, state != ST_ACTIVATE);
    lv_obj_set_flag(s_retry, LV_OBJ_FLAG_HIDDEN, state != ST_ERROR);
    switch (state) {
    case ST_NO_WIFI:
        set_status(T("Chưa có Internet", "No Internet"), COLOR_SUB);
        lv_label_set_text(s_hint, T("Trợ lý cần Internet. Hãy kết nối Wi-Fi hoặc bật dữ liệu di động trong Cài đặt.",
                                    "The assistant needs the Internet. Connect to Wi-Fi or turn on mobile data in Settings."));
        break;
    case ST_FETCHING:
        set_status(T("Đang kết nối máy chủ...", "Contacting the server..."), COLOR_SUB);
        lv_label_set_text(s_hint, "");
        break;
    case ST_ACTIVATE:
        set_status(T("Cần kích hoạt thiết bị", "The device must be activated"), COLOR_ACCENT);
        lv_label_set_text(s_hint, T("Vào xiaozhi.me, đăng nhập, mở Bảng điều khiển, chọn thêm thiết bị và nhập mã này. "
                                    "Màn hình sẽ tự chuyển khi kích hoạt xong.",
                                    "Open xiaozhi.me, sign in, open the console, add a device and enter this code. "
                                    "This page moves on by itself once the device is activated."));
        break;
    case ST_ERROR:
        set_status(T("Không kết nối được máy chủ", "Could not reach the server"), COLOR_ERROR);
        lv_label_set_text(s_hint, T("Kiểm tra kết nối Internet (Wi-Fi hoặc 4G) rồi thử lại.",
                                    "Check the Internet connection (Wi-Fi or 4G), then try again."));
        break;
    default:
        set_status("", COLOR_SUB);
        lv_label_set_text(s_hint, "");
        break;
    }
}

/* ---- chat ---- */

void add_bubble(const char *text, bool user, bool notice)
{
    if (s_bubble_count >= MAX_BUBBLES) {
        lv_obj_delete(lv_obj_get_child(s_bubbles, 0));
        s_bubble_count--;
    }
    lv_obj_t *row = lv_obj_create(s_bubbles);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, notice ? LV_FLEX_ALIGN_CENTER : (user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START),
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_max_width(label, lv_obj_get_content_width(s_bubbles) * 82 / 100, 0);
    lv_obj_set_style_radius(label, 16, 0);
    lv_obj_set_style_pad_hor(label, 14, 0);
    lv_obj_set_style_pad_ver(label, 8, 0);
    if (notice) {
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_ERROR), 0);
        lv_obj_set_style_text_font(label, phone_font_by_size(16), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(user ? COLOR_ACCENT : COLOR_CARD), 0);
        lv_obj_set_style_text_font(label, &phone_font_cjk_20, 0);     /* the assistant may answer in Chinese */
    }
    lv_label_set_text(label, text);
    s_bubble_count++;
    lv_obj_update_layout(s_bubbles);
    lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_ON);
}

void show_chat_state(int chat)
{
    const char *text = "";
    const char *symbol = nullptr;       /* none: the drawn microphone */
    uint32_t color = COLOR_ACCENT;
    bool enabled = true;
    switch (chat) {
    case ASSISTANT_CHAT_CONNECTING:
        text = T("Đang kết nối...", "Connecting...");
        enabled = false;
        color = 0x3A444E;
        break;
    case ASSISTANT_CHAT_IDLE:
        text = T("Chạm micro để nói", "Tap the microphone to talk");
        assistant_emote_set("neutral");
        break;
    case ASSISTANT_CHAT_OPENING:
        text = T("Đang mở kênh thoại...", "Opening the voice channel...");
        symbol = LV_SYMBOL_STOP;
        color = COLOR_LISTEN;
        break;
    case ASSISTANT_CHAT_LISTENING:
        text = T("Đang nghe...", "Listening...");
        symbol = LV_SYMBOL_STOP;
        color = COLOR_LISTEN;
        assistant_emote_set("idle");
        break;
    case ASSISTANT_CHAT_THINKING:
        text = T("Đang nghĩ...", "Thinking...");
        symbol = LV_SYMBOL_STOP;
        color = COLOR_LISTEN;
        assistant_emote_set("thinking");
        break;
    case ASSISTANT_CHAT_SPEAKING:
        text = T("Đang trả lời. Chạm để ngắt lời.", "Answering. Tap to interrupt.");
        symbol = LV_SYMBOL_VOLUME_MAX;
        color = COLOR_OK;
        break;
    case ASSISTANT_CHAT_ERROR:
        text = T("Không kết nối được máy chủ", "Could not reach the server");
        enabled = false;
        color = 0x3A444E;
        assistant_emote_set("sad");
        break;
    default:
        enabled = false;
        color = 0x3A444E;
        break;
    }
    lv_label_set_text(s_chat_status, text);
    lv_label_set_text(s_mic_label, symbol ? symbol : "");
    lv_obj_set_flag(s_mic_label, LV_OBJ_FLAG_HIDDEN, symbol == nullptr);
    lv_obj_set_flag(s_mic_icon, LV_OBJ_FLAG_HIDDEN, symbol != nullptr);
    lv_obj_set_style_bg_color(s_mic, lv_color_hex(color), 0);
    lv_obj_set_state(s_mic, LV_STATE_DISABLED, !enabled);
}

lv_obj_t *s_opts;       // options panel of the conversation page, opened by the gear

void on_gear(lv_event_t *)
{
    if (lv_obj_has_flag(s_opts, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_opts, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_opts, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The switch carries its preference key; the session applies it once no conversation is open */
void on_option(lv_event_t *e)
{
    const char *key = static_cast<const char *>(lv_event_get_user_data(e));
    phone_pref_set(key, lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? 1 : 0);
    assistant_chat_options_changed();
}

void add_option(lv_obj_t *panel, const char *text, const char *key)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, phone_font_by_size(18), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COLOR_ACCENT), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                              static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    if (phone_pref_get(key, 1)) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, on_option, LV_EVENT_VALUE_CHANGED, const_cast<char *>(key));
}

/* Echo delay: the panel closes so the result (a notice bubble) can be seen */
void on_calibrate(lv_event_t *)
{
    lv_obj_add_flag(s_opts, LV_OBJ_FLAG_HIDDEN);
    assistant_chat_calibrate();
}

void add_calibrate(lv_obj_t *panel)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, T("Đo độ trễ tiếng vọng (cho ngắt lời)", "Measure the echo delay (for interrupting)"));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, phone_font_by_size(18), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(btn, 16, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_add_event_cb(btn, on_calibrate, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, T("Đo", "Measure"));
    lv_obj_set_style_text_font(bl, phone_font_by_size(18), 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_center(bl);
}

void on_mic(lv_event_t *)
{
    assistant_chat_button();
}

/* Switch to the conversation page and connect */
void enter_chat()
{
    if (s_chat_started) {
        return;
    }
    s_chat_started = true;
    lv_obj_add_flag(s_setup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
    s_shown_chat = -1;
    assistant_chat_start(s_has_mqtt, s_has_websocket);
}

void leave_chat()
{
    if (!s_chat_started) {
        return;
    }
    s_chat_started = false;
    assistant_chat_stop();
}

void on_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    const int state = s_state;
    if (state == ST_ACTIVATE && strcmp(s_shown_code, s_code_text) != 0) {
        /* the code, spaced out so that it is easy to read aloud and to type */
        strlcpy(s_shown_code, s_code_text, sizeof(s_shown_code));
        char spaced[40] = "";
        size_t n = 0;
        for (const char *c = s_shown_code; *c && n + 3 < sizeof(spaced); c++) {
            spaced[n++] = *c;
            spaced[n++] = ' ';
        }
        spaced[n ? n - 1 : 0] = '\0';
        lv_label_set_text(s_code, spaced);
    }
    if (state != s_shown_state) {
        s_shown_state = state;
        if (state == ST_READY) {
            enter_chat();
        } else {
            show_setup_state(state);
        }
    }
    if (!s_chat_started) {
        return;
    }

    assistant_chat_msg_t msg;
    while (assistant_chat_poll(&msg)) {
        switch (msg.type) {
        case ASSISTANT_CHAT_MSG_USER:
            add_bubble(msg.text, true, false);
            break;
        case ASSISTANT_CHAT_MSG_ASSISTANT:
            add_bubble(msg.text, false, false);
            break;
        case ASSISTANT_CHAT_MSG_EMOTION:
            assistant_emote_set(msg.text);
            break;
        case ASSISTANT_CHAT_MSG_NOTICE:
            add_bubble(msg.text, false, true);
            break;
        }
    }
    const int chat = assistant_chat_state();
    if (chat != s_shown_chat) {
        s_shown_chat = chat;
        show_chat_state(chat);
    }
    /* the ring around the button follows the voice */
    const int level = chat == ASSISTANT_CHAT_LISTENING ? assistant_chat_mic_level() : 0;
    const int ring = BUTTON_D + 10 + level * 30 / 100;
    if (lv_obj_get_width(s_mic_ring) != ring) {
        lv_obj_set_size(s_mic_ring, ring, ring);
    }
    const bool talking = chat == ASSISTANT_CHAT_LISTENING || chat == ASSISTANT_CHAT_THINKING || chat == ASSISTANT_CHAT_SPEAKING;
    if (talking && !phone_lock_is_locked() && lv_obj_get_screen(s_root) == lv_screen_active()) {
        lv_display_trigger_activity(nullptr);       /* in a conversation: keep the screen on */
    }
}

void build_setup(lv_obj_t *root)
{
    s_setup = lv_obj_create(root);
    lv_obj_remove_style_all(s_setup);
    lv_obj_set_size(s_setup, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_setup, 16, 0);
    lv_obj_set_style_pad_bottom(s_setup, 16 + NAV_STRIP_H, 0);
    lv_obj_set_style_pad_row(s_setup, 16, 0);
    lv_obj_set_flex_flow(s_setup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_setup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_setup);
    lv_obj_set_style_text_font(title, phone_font_by_size(28), 0);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_text(title, T("Trợ lý AI", "AI Assistant"));

    s_status = lv_label_create(s_setup);
    lv_obj_set_style_text_font(s_status, phone_font_by_size(24), 0);
    lv_obj_set_style_margin_top(s_status, 60, 0);
    lv_label_set_text(s_status, "");

    s_code_card = lv_obj_create(s_setup);
    lv_obj_remove_style_all(s_code_card);
    lv_obj_set_size(s_code_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_code_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_code_card, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(s_code_card, 18, 0);
    lv_obj_set_style_pad_ver(s_code_card, 18, 0);
    lv_obj_set_flex_flow(s_code_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_code_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_code_card, 6, 0);
    lv_obj_remove_flag(s_code_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_code_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *code_title = lv_label_create(s_code_card);
    lv_obj_set_style_text_color(code_title, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(code_title, phone_font_by_size(18), 0);
    lv_label_set_text(code_title, T("Mã kích hoạt", "Activation code"));
    s_code = lv_label_create(s_code_card);
    lv_obj_set_style_text_font(s_code, &lv_font_montserrat_44, 0);
    lv_label_set_text(s_code, "");

    s_hint = lv_label_create(s_setup);
    lv_obj_set_width(s_hint, LV_PCT(100));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_hint, phone_font_by_size(18), 0);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_hint, 6, 0);
    lv_label_set_text(s_hint, "");

    s_retry = lv_button_create(s_setup);
    lv_obj_remove_style_all(s_retry);
    lv_obj_set_size(s_retry, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_pad_hor(s_retry, 32, 0);
    lv_obj_set_style_radius(s_retry, 16, 0);
    lv_obj_set_style_bg_opa(s_retry, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_retry, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_retry, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_retry, on_retry, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_retry, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *rl = lv_label_create(s_retry);
    lv_label_set_text(rl, T("Thử lại", "Retry"));
    lv_obj_center(rl);
}

/* LVGL has no microphone symbol: a capsule in a cradle, on a stand */
lv_obj_t *make_mic_icon(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 34, 42);
    lv_obj_remove_flag(icon, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto part = [icon](int x, int y, int w, int h, int radius) {
        lv_obj_t *o = lv_obj_create(icon);
        lv_obj_remove_style_all(o);
        lv_obj_set_pos(o, x, y);
        lv_obj_set_size(o, w, h);
        lv_obj_set_style_radius(o, radius, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_white(), 0);
        lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        return o;
    };
    part(10, 0, 14, 24, 7);                     /* capsule */
    lv_obj_t *cradle = part(3, 6, 28, 26, 14);  /* open ring: the lower half of a rounded border */
    lv_obj_set_style_bg_opa(cradle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cradle, 3, 0);
    lv_obj_set_style_border_color(cradle, lv_color_white(), 0);
    lv_obj_set_style_border_side(cradle, static_cast<lv_border_side_t>(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT |
                                 LV_BORDER_SIDE_RIGHT), 0);
    part(15, 31, 4, 7, 0);                      /* stem */
    part(8, 38, 18, 4, 2);                      /* foot */
    return icon;
}

void build_chat(lv_obj_t *root, int root_w, int root_h)
{
    constexpr int FACE_Y = -36;         // the animations have empty space above the eyes
    constexpr int STATUS_Y = ASSISTANT_EMOTE_SIZE + FACE_Y - 46;
    constexpr int BUBBLES_Y = STATUS_Y + 34;
    const int controls_h = BUTTON_D + 40 + 12;      // button with its ring, then a gap above the strip

    s_chat = lv_obj_create(root);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, root_w, root_h);
    lv_obj_remove_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *face = assistant_emote_create(s_chat, COLOR_BG);
    lv_obj_align(face, LV_ALIGN_TOP_MID, 0, FACE_Y);

    s_chat_status = lv_label_create(s_chat);
    lv_obj_set_style_text_color(s_chat_status, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_chat_status, phone_font_by_size(18), 0);
    lv_obj_align(s_chat_status, LV_ALIGN_TOP_MID, 0, STATUS_Y);
    lv_label_set_text(s_chat_status, "");

    s_bubbles = lv_obj_create(s_chat);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_size(s_bubbles, root_w, root_h - BUBBLES_Y - controls_h - NAV_STRIP_H);
    lv_obj_set_y(s_bubbles, BUBBLES_Y);
    lv_obj_set_style_pad_hor(s_bubbles, 14, 0);
    lv_obj_set_style_pad_ver(s_bubbles, 6, 0);
    lv_obj_set_style_pad_row(s_bubbles, 8, 0);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bubbles, LV_DIR_VER);
    phone_ui_scrollbar(s_bubbles);
    s_bubble_count = 0;

    /* microphone button, in a ring that grows with the voice */
    lv_obj_t *holder = lv_obj_create(s_chat);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, BUTTON_D + 40, BUTTON_D + 40);
    lv_obj_align(holder, LV_ALIGN_BOTTOM_MID, 0, -(NAV_STRIP_H + 6));
    lv_obj_remove_flag(holder, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    s_mic_ring = lv_obj_create(holder);
    lv_obj_remove_style_all(s_mic_ring);
    lv_obj_set_size(s_mic_ring, BUTTON_D + 10, BUTTON_D + 10);
    lv_obj_center(s_mic_ring);
    lv_obj_set_style_radius(s_mic_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_mic_ring, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(s_mic_ring, lv_color_hex(COLOR_LISTEN), 0);
    lv_obj_remove_flag(s_mic_ring, LV_OBJ_FLAG_CLICKABLE);
    s_mic = lv_button_create(holder);
    lv_obj_remove_style_all(s_mic);
    lv_obj_set_size(s_mic, BUTTON_D, BUTTON_D);
    lv_obj_center(s_mic);
    lv_obj_set_ext_click_area(s_mic, 16);
    lv_obj_set_style_radius(s_mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_mic, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_mic, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_mic, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_mic, on_mic, LV_EVENT_CLICKED, nullptr);
    s_mic_label = lv_label_create(s_mic);
    lv_obj_set_style_text_font(s_mic_label, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_mic_label, "");
    lv_obj_center(s_mic_label);
    lv_obj_add_flag(s_mic_label, LV_OBJ_FLAG_HIDDEN);
    s_mic_icon = make_mic_icon(s_mic);
    lv_obj_center(s_mic_icon);

    /* options: last, so the panel is drawn over the face and the bubbles */
    lv_obj_t *gear = lv_button_create(s_chat);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 44, 44);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_ext_click_area(gear, 10);
    lv_obj_add_event_cb(gear, on_gear, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *gear_icon = lv_label_create(gear);
    lv_label_set_text(gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gear_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(gear_icon, lv_color_hex(COLOR_SUB), 0);
    lv_obj_center(gear_icon);
    s_opts = lv_obj_create(s_chat);
    lv_obj_remove_style_all(s_opts);
    lv_obj_set_width(s_opts, root_w * 80 / 100);
    lv_obj_set_height(s_opts, LV_SIZE_CONTENT);
    lv_obj_align(s_opts, LV_ALIGN_TOP_RIGHT, -8, 56);
    lv_obj_set_style_bg_opa(s_opts, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_opts, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_radius(s_opts, 14, 0);
    lv_obj_set_style_pad_all(s_opts, 16, 0);
    lv_obj_set_style_pad_row(s_opts, 14, 0);
    lv_obj_set_flex_flow(s_opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_opts, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_opts, LV_OBJ_FLAG_HIDDEN);
    add_option(s_opts, T("Gọi \"Hi ESP\" để bắt đầu", "Say \"Hi ESP\" to start"), "wake");
    add_option(s_opts, T("Nói \"Hi ESP\" để ngắt lời", "Say \"Hi ESP\" to interrupt"), "barge");
    add_calibrate(s_opts);
}

void build_ui()
{
    s_shown_state = -1;
    s_shown_chat = -1;
    s_shown_code[0] = '\0';
    s_chat_started = false;

    lv_obj_t *root = lv_obj_create(lv_screen_active());
    s_root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_font(root, phone_font_by_size(20), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, [](lv_event_t *) {
        s_root = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);

    build_setup(root);
    build_chat(root, lv_obj_get_width(root), lv_obj_get_height(root));
}

} // namespace

namespace esp_brookesia::apps {

PhoneAssistant *PhoneAssistant::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneAssistant();
    }
    return _instance;
}

PhoneAssistant::PhoneAssistant():
    App(phone_tr("Trợ lý AI", "AI Assistant"), &phone_assistant_icon, true, true, true)
{
}

PhoneAssistant::~PhoneAssistant()
{
}

bool PhoneAssistant::run()
{
    build_ui();
    lv_timer_create(on_timer, 100, nullptr);
    if (s_state != ST_READY) {
        start_info_task();
    }
    return true;
}

/* Back ends a conversation first, then leaves the app */
bool PhoneAssistant::back()
{
    const int chat = assistant_chat_state();
    if (s_chat_started && (chat == ASSISTANT_CHAT_LISTENING || chat == ASSISTANT_CHAT_THINKING ||
                           chat == ASSISTANT_CHAT_SPEAKING || chat == ASSISTANT_CHAT_OPENING)) {
        if (chat == ASSISTANT_CHAT_SPEAKING) {
            assistant_chat_button();    /* cut the answer, then the second press below ends the conversation */
        }
        assistant_chat_button();
        return true;
    }
    return notifyCoreClosed();
}

/* The setup task may be inside an HTTPS request: it is told to stop and ends by itself */
bool PhoneAssistant::close()
{
    ++s_generation;
    leave_chat();
    assistant_emote_destroy();
    if (s_state != ST_READY) {
        s_state = ST_IDLE;
    }
    return true;
}

/* In the background: the conversation ends and the server connection is closed */
bool PhoneAssistant::pause()
{
    leave_chat();
    return true;
}

/* Back in front: connect again, or check the activation again */
bool PhoneAssistant::resume()
{
    if (!s_root) {
        return true;
    }
    if (s_state == ST_READY) {
        enter_chat();
    } else if (!s_task_running) {
        start_info_task();
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneAssistant, APP_NAME, []()
{
    return std::shared_ptr<PhoneAssistant>(PhoneAssistant::requestInstance(), [](PhoneAssistant * p) {});
})

} // namespace esp_brookesia::apps
