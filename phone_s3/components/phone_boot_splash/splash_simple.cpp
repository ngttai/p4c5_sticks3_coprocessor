/*
 * PHONE_BOOT_SPLASH_SIMPLE: a hand-drawn wavy line draws itself, then a greeting ("Xin chào!" or
 * "Hello!", by the phone language) fades in under it. After the boot splash of esp_chatbot_v1,
 * sized for the 480 x 800 screen; no image, the line is drawn by LVGL.
 * The greeting font (Be Vietnam Pro 56 px, only its letters): ../phone_app_settings/tools/gen_lv_font.py
 * FONT.ttf 56 splash_font_56 splash_font_56.c "Xin chào!Hello!"
 */
#include "phone_boot_splash.h"
#include <cmath>
#include "esp_log.h"
#include "lvgl.h"
#include "phone_locale.h"

LV_FONT_DECLARE(splash_font_56)   // splash_font_56.c, the greeting only

namespace {

const char *TAG = "splash";

constexpr uint32_t COLOR_BG = 0x0E1116;
constexpr uint32_t COLOR_FG = 0xF3F4F6;
constexpr float SCALE = 2.6f;           // the line is drawn in a 140 x 50 box, scaled to the screen
constexpr int LINE_W = static_cast<int>(140 * SCALE);
constexpr int LINE_H = static_cast<int>(50 * SCALE);
constexpr int LINE_PX = 8;
constexpr int SEG_POINTS = 24;          // points per curve
constexpr int CURVES = 3;
constexpr int POINTS = CURVES * SEG_POINTS + 1;
constexpr int GAP = 12;                 // between the line and the greeting
constexpr uint32_t STEP_MS = 30;        // one step of the line or of the fades per call
constexpr int STEP_POINTS = 3;          // points of the line added per step: 24 steps
constexpr int FADE_STEPS = 12;
constexpr uint32_t HOLD_MS = 700;
constexpr uint32_t FADE_OUT_MS = 300;

lv_obj_t *s_root;
lv_obj_t *s_line;
lv_obj_t *s_text;
lv_timer_t *s_timer;
lv_point_t s_pts[POINTS];
int s_revealed = 1;                     // points of the line drawn (two make the first segment)
int s_fade;
bool s_ready;
uint32_t s_start;
uint32_t s_done_at;

/* The flourish, in the 140 x 50 box: three cubic curves, each one leaving the last one smoothly
 * (SVG "M5 30 C20 5 30 45 45 25 S70 5 80 28 S105 45 135 15") */
constexpr float CURVE[CURVES][4][2] = {
    {{5, 30}, {20, 5}, {30, 45}, {45, 25}},
    {{45, 25}, {60, 5}, {70, 5}, {80, 28}},
    {{80, 28}, {90, 51}, {105, 45}, {135, 15}},
};

void build_points()
{
    int n = 0;
    for (int c = 0; c < CURVES; c++) {
        const auto &p = CURVE[c];
        for (int i = (c == 0 ? 0 : 1); i <= SEG_POINTS; i++) {
            const float t = static_cast<float>(i) / SEG_POINTS;
            const float u = 1 - t;
            const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
            const float x = b0 * p[0][0] + b1 * p[1][0] + b2 * p[2][0] + b3 * p[3][0];
            const float y = b0 * p[0][1] + b1 * p[1][1] + b2 * p[2][1] + b3 * p[3][1];
            s_pts[n].x = static_cast<int32_t>(lroundf(x * SCALE));
            s_pts[n].y = static_cast<int32_t>(lroundf(y * SCALE));
            n++;
        }
    }
}

void draw_line(lv_event_t *e)
{
    if (s_revealed < 2) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_line, &a);
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(COLOR_FG);
    dsc.width = LINE_PX;
    dsc.round_start = 1;
    dsc.round_end = 1;
    for (int i = 0; i < s_revealed - 1; i++) {
        dsc.p1.x = a.x1 + s_pts[i].x;
        dsc.p1.y = a.y1 + s_pts[i].y;
        dsc.p2.x = a.x1 + s_pts[i + 1].x;
        dsc.p2.y = a.y1 + s_pts[i + 1].y;
        lv_draw_line(layer, &dsc);
    }
}

void finish(lv_anim_t *)
{
    lv_timer_delete(s_timer);
    s_timer = nullptr;
    lv_obj_delete(s_root);
    s_root = nullptr;
    ESP_LOGI(TAG, "done");
}

void fade_out()
{
    lv_timer_pause(s_timer);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FADE_OUT_MS);
    lv_anim_set_completed_cb(&a, finish);
    lv_anim_start(&a);
}

/* Waits for the phone to be ready (the boot holds the LVGL lock for about a second), then one step
 * per call: the line, then the greeting, then a pause and the fade out */
void on_timer(lv_timer_t *)
{
    if (lv_obj_get_parent(s_root) != lv_layer_sys()) {
        lv_obj_set_parent(s_root, lv_layer_sys());
    }
    lv_obj_move_foreground(s_root);     // over the lock screen, made later on the same layer
    lv_display_trigger_activity(nullptr);
    if (!s_ready) {
        return;
    }
    if (s_revealed < POINTS) {
        /* by steps, not by time: the launcher's first draws hold the timer, a time based line
         * would be drawn whole by then */
        if (s_start == 0) {
            s_start = lv_tick_get() | 1;
        }
        s_revealed = s_revealed + STEP_POINTS < POINTS ? s_revealed + STEP_POINTS : POINTS;
        lv_obj_invalidate(s_line);
        if (s_revealed == POINTS) {
            ESP_LOGI(TAG, "line drawn in %u ms", static_cast<unsigned>(lv_tick_elaps(s_start)));
        }
        return;
    }
    if (s_fade < FADE_STEPS) {
        s_fade++;
        lv_obj_set_style_opa(s_text, static_cast<lv_opa_t>(s_fade * LV_OPA_COVER / FADE_STEPS), 0);
        if (s_fade == FADE_STEPS) {
            s_done_at = lv_tick_get();
        }
        return;
    }
    if (lv_tick_elaps(s_done_at) >= HOLD_MS) {
        fade_out();
    }
}

} // namespace

extern "C" void phone_boot_splash_show(void)
{
    if (s_root) {
        return;
    }
    s_root = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COLOR_BG), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);         // nothing under it takes a touch
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(s_root);
    const int w = lv_obj_get_width(s_root);
    const int h = lv_obj_get_height(s_root);

    build_points();
    s_line = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_line);
    lv_obj_set_size(s_line, LINE_W, LINE_H);
    lv_obj_remove_flag(s_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_line, draw_line, LV_EVENT_DRAW_MAIN, nullptr);

    s_text = lv_label_create(s_root);
    lv_label_set_text(s_text, phone_lang_get() == PHONE_LANG_VI ? "Xin chào!" : "Hello!");
    lv_obj_set_style_text_font(s_text, &splash_font_56, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(COLOR_FG), 0);
    lv_obj_set_style_opa(s_text, LV_OPA_TRANSP, 0);

    /* positions set once: a live alignment is not needed for a screen that only fades */
    lv_obj_update_layout(s_text);
    const int text_w = lv_obj_get_width(s_text);
    const int text_h = lv_obj_get_height(s_text);
    const int top = (h - (LINE_H + GAP + text_h)) / 2;
    lv_obj_set_pos(s_line, (w - LINE_W) / 2, top);
    lv_obj_set_pos(s_text, (w - text_w) / 2, top + LINE_H + GAP);

    s_timer = lv_timer_create(on_timer, STEP_MS, nullptr);
}

extern "C" void phone_boot_splash_raise(void)
{
    if (s_root) {
        lv_obj_set_parent(s_root, lv_layer_sys());
        lv_obj_move_foreground(s_root);
    }
}

extern "C" void phone_boot_splash_ready(void)
{
    s_ready = true;
}
