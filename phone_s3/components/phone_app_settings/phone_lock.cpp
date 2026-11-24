#include "phone_lock.h"
#include <atomic>
#include <cstdio>
#include <ctime>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_net.h"
#include "phone_power.h"

#define TAG "LOCK"

LV_FONT_DECLARE(phone_font_clock_120);
/* Launcher wallpaper of the 480x800 dark stylesheet (shown centered on 0x1A1A1A, as on the launcher) */
LV_IMAGE_DECLARE(esp_brookesia_image_middle_wallpaper_dark_480_480);

/* Status bar icons of the same stylesheet, so the lock screen matches the status bar */
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_wifi_close_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_wifi_level1_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_wifi_level2_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_wifi_level3_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_battery_level1_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_battery_level2_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_battery_level3_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_battery_level4_24_24);
LV_IMAGE_DECLARE(esp_brookesia_image_middle_status_bar_battery_charge_24_24);

namespace {

/* ---------------- policy ---------------- */

constexpr int DIM_PERCENT = 2;                 // below the 5 % minimum of the brightness setting
constexpr uint32_t LOCK_IDLE_MS = 10000;       // lock screen left alone -> screen off
constexpr uint32_t DIM_LEAD_MAX_MS = 10000;    // dim this long before the screen goes off
constexpr uint32_t IDLE_TICK_MS = 200;
constexpr uint32_t FIRST_FRAME_MS = 60;        // let LVGL draw the lock screen before the backlight comes on

constexpr int TIMEOUTS_S[] = {15, 30, 60, 120, 300, 0};
constexpr int TIMEOUT_DEFAULT = 1;

enum State : int { ST_ACTIVE, ST_DIM, ST_LOCKED, ST_OFF };

enum : uint32_t {
    NB_KEY = BIT0,      // power key short press
    NB_OFF = BIT1,      // idle timer asks for screen off
    NB_WAKE = BIT2,     // wake to the lock screen (phone_lock_wake / charger plugged)
};

std::atomic<int> s_state{ST_ACTIVE};
TaskHandle_t s_worker;
SemaphoreHandle_t s_wake_done;
bool s_paused;              // worker task only
uint32_t s_last_idle;       // LVGL task only

uint32_t timeout_ms()
{
    int i = phone_lock_timeout_get();
    return static_cast<uint32_t>(TIMEOUTS_S[i]) * 1000U;
}

int user_brightness()
{
    return phone_pref_get("bri", 100);
}

void wifi_power_save(bool on)
{
    if (phone_net_ready()) {
        esp_wifi_set_ps(on ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    }
}

/* ---------------- blurred wallpaper (built once at boot) ---------------- */

constexpr int BLUR_RADIUS = 12;
constexpr int BLUR_PASSES = 3;       // three box blurs approximate a Gaussian
constexpr int DARKEN_PCT = 65;       // keep 65 % of the brightness so white text stays readable

lv_image_dsc_t s_wall_dsc;
std::atomic<bool> s_wall_ready{false};

/* One box blur pass over n pixels of 3 bytes, step bytes apart, with edge clamping. */
void box_blur_line(uint8_t *px, int n, int step, uint8_t *tmp)
{
    for (int i = 0; i < n; i++) {
        memcpy(tmp + i * 3, px + i * step, 3);
    }
    const int win = 2 * BLUR_RADIUS + 1;
    for (int c = 0; c < 3; c++) {
        int sum = 0;
        for (int k = -BLUR_RADIUS; k <= BLUR_RADIUS; k++) {
            sum += tmp[LV_CLAMP(0, k, n - 1) * 3 + c];
        }
        for (int i = 0; i < n; i++) {
            px[i * step + c] = static_cast<uint8_t>(sum / win);
            sum += tmp[LV_MIN(i + BLUR_RADIUS + 1, n - 1) * 3 + c] - tmp[LV_MAX(i - BLUR_RADIUS, 0) * 3 + c];
        }
    }
}

void blur_task(void *)
{
    const lv_image_dsc_t *src = &esp_brookesia_image_middle_wallpaper_dark_480_480;
    const int sw = src->header.w;
    const int sh = src->header.h;
    const int w = BSP_LCD_H_RES;
    const int h = BSP_LCD_V_RES;
    const int stride = w * 3;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(stride * h, MALLOC_CAP_SPIRAM));
    uint8_t *tmp = static_cast<uint8_t *>(heap_caps_malloc(LV_MAX(w, h) * 3, MALLOC_CAP_DEFAULT));
    if (!buf || !tmp || src->header.cf != LV_COLOR_FORMAT_RGB888) {
        ESP_LOGW(TAG, "Wallpaper blur skipped");
        heap_caps_free(buf);
        heap_caps_free(tmp);
        vTaskDelete(nullptr);
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    /* Scale the wallpaper to cover the whole screen (center crop, nearest pixel); the blur hides the steps */
    const int crop_w = LV_MIN(sw, sh * w / h);
    const int crop_h = LV_MIN(sh, sw * h / w);
    const int ox = (sw - crop_w) / 2;
    const int oy = (sh - crop_h) / 2;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = src->data + (oy + y * crop_h / h) * sw * 3;
        uint8_t *dst = buf + y * stride;
        for (int x = 0; x < w; x++) {
            memcpy(dst + x * 3, row + (ox + x * crop_w / w) * 3, 3);
        }
    }
    for (int pass = 0; pass < BLUR_PASSES; pass++) {
        for (int y = 0; y < h; y++) {
            box_blur_line(buf + y * stride, w, 3, tmp);
        }
        for (int x = 0; x < w; x++) {
            box_blur_line(buf + x * 3, h, stride, tmp);
        }
    }
    /* Darkened and stored as RGB565, the format of the display: drawn as a plain copy instead of a conversion
     * of each pixel at every redraw (in place: pixel i is read at 3 i before 2 i is written) */
    auto *out = reinterpret_cast<uint16_t *>(buf);
    for (int i = 0; i < w * h; i++) {
        const int b = buf[i * 3] * DARKEN_PCT / 100;
        const int g = buf[i * 3 + 1] * DARKEN_PCT / 100;
        const int r = buf[i * 3 + 2] * DARKEN_PCT / 100;
        out[i] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    heap_caps_free(tmp);

    s_wall_dsc = *src;
    s_wall_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_wall_dsc.header.w = w;
    s_wall_dsc.header.h = h;
    s_wall_dsc.header.stride = w * 2;
    s_wall_dsc.data_size = w * 2 * h;
    s_wall_dsc.data = buf;
    s_wall_ready = true;
    ESP_LOGI(TAG, "Wallpaper blurred in %d ms", static_cast<int>((esp_timer_get_time() - t0) / 1000));
    vTaskDelete(nullptr);
}

/* ---------------- lock screen UI (LVGL context) ---------------- */

constexpr int DIGITS = 4;              // HH MM
constexpr int DIGIT_W = 88;
constexpr int DIGIT_H = 148;
constexpr int DIGIT_HALF = DIGIT_H / 2;
constexpr int PAIR_GAP = 6;
constexpr int GROUP_GAP = 32;
constexpr int32_t FLIP_MAX = DIGIT_HALF * 2;
constexpr uint32_t FLIP_MS = 480;
constexpr uint32_t INTRO_DELAY_MS = 150;       // lock screen shown: the hour pair starts flipping after this
constexpr uint32_t INTRO_STAGGER_MS = 150;     // then the minute pair
constexpr uint32_t CARD_BG = 0x1C1C1E;
constexpr uint32_t CARD_BG_TOP = 0x2A2A2E;
constexpr uint32_t HINGE = 0x0A0A0A;
constexpr uint32_t DIGIT_COLOR = 0xF5F5F7;
constexpr uint32_t SUB_COLOR = 0xC7CBD1;

constexpr int TRACK_W = 420;
constexpr int TRACK_H = 76;
constexpr int KNOB_W = 104;
constexpr int KNOB_PAD = 6;
constexpr int KNOB_TRAVEL = TRACK_W - KNOB_W - 2 * KNOB_PAD;

constexpr int FX_W = 440;
constexpr int FX_H = 300;
constexpr int PARTICLES = 44;
constexpr uint32_t FX_BLUE_SOFT = 0x59B2FF;
constexpr uint32_t FX_BLUE_BRIGHT = 0x9AD0FF;
constexpr uint32_t FX_TICK_MS = 33;

struct FlipDigit {
    lv_obj_t *card, *top_lbl, *bot_lbl, *flap, *flap_lbl, *hinge, *shade;
    int32_t label_ofs_y;
    char current, target, pending, old_ch;
    bool flap_lower, animating;
};

struct Particle {
    float x, y, vx, vy, size, life, decay;
    uint32_t color;
};

struct Ui {
    lv_obj_t *root;
    FlipDigit digits[DIGITS];
    lv_obj_t *date;
    lv_obj_t *wifi_icon;
    lv_obj_t *bat_label;
    lv_obj_t *bat_icon;
    lv_obj_t *track, *knob, *hint;
    lv_timer_t *clock_timer;
    bool clock_primed;
    lv_obj_t *fx_root;
    Particle particles[PARTICLES];
    lv_obj_t *fx_tip;          // "Charging  57%"
    lv_timer_t *fx_tick;
    uint32_t rng;
};

Ui s_ui;

void set_char(lv_obj_t *lbl, char ch)
{
    char buf[2] = {ch, 0};
    lv_label_set_text(lbl, buf);
}

/* progress 0: upper leaf open (old digit); DIGIT_HALF: at the hinge; FLIP_MAX: lower leaf open (new digit) */
void flip_progress(FlipDigit *d, int32_t p)
{
    p = LV_CLAMP(0, p, FLIP_MAX);
    const bool lower = p > DIGIT_HALF;
    if (lower != d->flap_lower) {
        d->flap_lower = lower;
        set_char(d->flap_lbl, lower ? d->target : d->old_ch);
        lv_obj_set_style_bg_color(d->flap, lv_color_hex(lower ? CARD_BG : CARD_BG_TOP), 0);
    }
    int32_t h, y, lbl_y;
    if (!lower) {
        h = LV_MAX(DIGIT_HALF - p, 1);
        y = DIGIT_HALF - h;
        lbl_y = d->label_ofs_y - y;
    } else {
        h = LV_MAX(p - DIGIT_HALF, 1);
        y = DIGIT_HALF;
        lbl_y = d->label_ofs_y - DIGIT_HALF;
    }
    lv_obj_set_pos(d->flap, 0, y);
    lv_obj_set_height(d->flap, h);
    lv_obj_set_y(d->flap_lbl, lbl_y);
    const int32_t dist = lower ? (FLIP_MAX - p) : p;
    lv_obj_set_pos(d->shade, 0, y);
    lv_obj_set_height(d->shade, h);
    lv_obj_set_style_bg_opa(d->shade, static_cast<lv_opa_t>(dist * 160 / DIGIT_HALF), 0);
    lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
}

void flip_start(FlipDigit *d, char next, uint32_t delay_ms = 0, bool force = false);

void flip_finish(FlipDigit *d)
{
    set_char(d->top_lbl, d->target);
    set_char(d->bot_lbl, d->target);
    d->current = d->target;
    d->target = 0;
    d->flap_lower = false;
    d->animating = false;
    lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
    char queued = d->pending;
    d->pending = 0;
    if (queued && queued != d->current) {
        flip_start(d, queued);
    }
}

/* force: flip even when the digit does not change (intro) */
void flip_start(FlipDigit *d, char next, uint32_t delay_ms, bool force)
{
    if (d->animating) {
        d->pending = next;
        return;
    }
    if (next == d->current && !force) {
        return;
    }
    d->old_ch = d->current;
    d->target = next;
    d->animating = true;
    d->flap_lower = false;
    set_char(d->top_lbl, next);
    set_char(d->bot_lbl, d->old_ch);
    set_char(d->flap_lbl, d->old_ch);
    lv_obj_set_style_bg_color(d->flap, lv_color_hex(CARD_BG_TOP), 0);
    lv_obj_remove_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
    flip_progress(d, 0);
    lv_obj_move_foreground(d->shade);
    lv_obj_move_foreground(d->hinge);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, d);
    lv_anim_set_values(&a, 0, FLIP_MAX);
    lv_anim_set_duration(&a, FLIP_MS);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, [](void *var, int32_t v) {
        flip_progress(static_cast<FlipDigit *>(var), v);
    });
    lv_anim_set_completed_cb(&a, [](lv_anim_t *anim) {
        flip_finish(static_cast<FlipDigit *>(anim->var));
    });
    lv_anim_start(&a);
}

void flip_set(FlipDigit *d, char ch, bool animate)
{
    if (!animate) {
        lv_anim_delete(d, nullptr);
        d->pending = d->target = 0;
        d->animating = d->flap_lower = false;
        set_char(d->top_lbl, ch);
        set_char(d->bot_lbl, ch);
        d->current = ch;
        lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    flip_start(d, ch);
}

lv_obj_t *half_clip(lv_obj_t *parent, int y, uint32_t bg)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, DIGIT_W, DIGIT_HALF);
    lv_obj_set_pos(c, 0, y);
    lv_obj_set_style_bg_color(c, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_remove_flag(c, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return c;
}

lv_obj_t *digit_label(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "0");
    lv_obj_set_style_text_font(l, &phone_font_clock_120, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(DIGIT_COLOR), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, DIGIT_W);
    lv_obj_set_pos(l, 0, y);
    return l;
}

void make_digit(lv_obj_t *parent, FlipDigit *d)
{
    *d = {};
    d->label_ofs_y = (DIGIT_H - phone_font_clock_120.line_height) / 2;
    d->card = lv_obj_create(parent);
    lv_obj_remove_style_all(d->card);
    lv_obj_set_size(d->card, DIGIT_W, DIGIT_H);
    lv_obj_set_style_radius(d->card, 12, 0);
    lv_obj_set_style_bg_color(d->card, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_bg_opa(d->card, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(d->card, true, 0);
    lv_obj_remove_flag(d->card, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    d->top_lbl = digit_label(half_clip(d->card, 0, CARD_BG_TOP), d->label_ofs_y);
    d->bot_lbl = digit_label(half_clip(d->card, DIGIT_HALF, CARD_BG), d->label_ofs_y - DIGIT_HALF);
    d->flap = half_clip(d->card, 0, CARD_BG_TOP);
    d->flap_lbl = digit_label(d->flap, d->label_ofs_y);
    lv_obj_add_flag(d->flap, LV_OBJ_FLAG_HIDDEN);

    d->shade = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->shade);
    lv_obj_set_size(d->shade, DIGIT_W, DIGIT_HALF);
    lv_obj_set_style_bg_color(d->shade, lv_color_black(), 0);
    lv_obj_remove_flag(d->shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(d->shade, LV_OBJ_FLAG_HIDDEN);

    d->hinge = lv_obj_create(d->card);
    lv_obj_remove_style_all(d->hinge);
    lv_obj_set_size(d->hinge, DIGIT_W, 3);
    lv_obj_set_pos(d->hinge, 0, DIGIT_HALF - 1);
    lv_obj_set_style_bg_color(d->hinge, lv_color_hex(HINGE), 0);
    lv_obj_set_style_bg_opa(d->hinge, LV_OPA_COVER, 0);
    d->current = '0';
}

/* ---- charging effect: particles rising from the bottom ----
 * Runs while the lock screen is on and the battery charges: started by the clock tick (every second, and when
 * the lock screen is shown), stopped by it once charging ends (full, or the cable is out), and removed before the
 * screen goes off (nothing is drawn with the backlight off). */

uint32_t fx_rand()
{
    uint32_t x = s_ui.rng ? s_ui.rng : 1;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_ui.rng = x;
    return x;
}

float fx_rand01()
{
    return static_cast<float>(fx_rand() & 0xFFFF) / 65535.0f;
}

void fx_respawn(Particle *p)
{
    p->x = FX_W * 0.5f + (fx_rand01() - 0.5f) * 240.0f;
    p->y = FX_H - 8.0f - fx_rand01() * 36.0f;
    p->vx = (fx_rand01() - 0.5f) * 2.2f;
    p->vy = -(1.6f + fx_rand01() * 3.8f);
    p->size = 3.0f + fx_rand01() * 9.0f;
    p->life = 0.7f + fx_rand01() * 0.3f;
    p->decay = 0.014f + fx_rand01() * 0.022f;
    p->color = (fx_rand() & 1) ? FX_BLUE_BRIGHT : FX_BLUE_SOFT;
}

/* Where a particle is drawn, relative to the effect area */
lv_area_t fx_area(const Particle &p)
{
    const int sz = LV_MAX(static_cast<int>(p.size * (0.55f + 0.45f * p.life)), 2);
    lv_area_t a;
    a.x1 = static_cast<int32_t>(p.x - sz * 0.5f);
    a.y1 = static_cast<int32_t>(p.y - sz * 0.5f);
    a.x2 = a.x1 + sz - 1;
    a.y2 = a.y1 + sz - 1;
    return a;
}

/* The particles are drawn here, not as objects: 44 objects invalidated each tick went past the 32 areas LVGL
 * keeps (LV_INV_BUF_SIZE), so every tick redrew the whole screen (core 0 over 90 %) */
void fx_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t box;
    lv_obj_get_coords(s_ui.fx_root, &box);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = LV_RADIUS_CIRCLE;
    for (const Particle &p : s_ui.particles) {
        lv_area_t a = fx_area(p);
        lv_area_move(&a, box.x1, box.y1);
        dsc.bg_color = lv_color_hex(p.color);
        dsc.bg_opa = static_cast<lv_opa_t>(LV_CLAMP(0, static_cast<int>(p.life * 220.0f), 220));
        lv_draw_rect(layer, &dsc, &a);
    }
}

void fx_stop()
{
    if (s_ui.fx_tick) {
        lv_timer_delete(s_ui.fx_tick);
        s_ui.fx_tick = nullptr;
    }
    if (s_ui.fx_root) {
        lv_obj_delete(s_ui.fx_root);
        s_ui.fx_root = nullptr;
        s_ui.fx_tip = nullptr;
    }
}

void area_grow(lv_area_t *dst, const lv_area_t &a)
{
    dst->x1 = LV_MIN(dst->x1, a.x1);
    dst->y1 = LV_MIN(dst->y1, a.y1);
    dst->x2 = LV_MAX(dst->x2, a.x2);
    dst->y2 = LV_MAX(dst->y2, a.y2);
}

void fx_tick(lv_timer_t *)
{
    /* Each particle dirties where it was and where it is now. A single box around all of them is most of the
     * effect area every tick; one box per cell of a 4 x 4 grid keeps the redraw small, and two ticks before one
     * redraw stay under the 32 areas of LVGL (over that it redraws the whole screen) */
    constexpr int GRID = 4;
    lv_area_t cells[GRID * GRID];
    bool used[GRID * GRID] = {};
    for (Particle &p : s_ui.particles) {
        lv_area_t a = fx_area(p);
        p.x += p.vx;
        p.y += p.vy;
        p.vx *= 0.992f;
        p.vy -= 0.035f;
        p.life -= p.decay;
        if (p.life <= 0.0f || p.y < -20.0f) {
            fx_respawn(&p);
            const lv_area_t born = fx_area(p);
            const int bc = LV_CLAMP(0, static_cast<int>(p.x) * GRID / FX_W, GRID - 1) +
                           GRID * LV_CLAMP(0, static_cast<int>(p.y) * GRID / FX_H, GRID - 1);
            if (used[bc]) {
                area_grow(&cells[bc], born);
            } else {
                cells[bc] = born;
                used[bc] = true;
            }
        } else {
            const lv_area_t now = fx_area(p);
            area_grow(&a, now);
        }
        const int c = LV_CLAMP(0, static_cast<int>((a.x1 + a.x2) / 2) * GRID / FX_W, GRID - 1) +
                      GRID * LV_CLAMP(0, static_cast<int>((a.y1 + a.y2) / 2) * GRID / FX_H, GRID - 1);
        if (used[c]) {
            area_grow(&cells[c], a);
        } else {
            cells[c] = a;
            used[c] = true;
        }
    }
    lv_area_t box;
    lv_obj_get_coords(s_ui.fx_root, &box);
    for (int i = 0; i < GRID * GRID; i++) {
        if (used[i]) {
            lv_area_move(&cells[i], box.x1, box.y1);
            lv_obj_invalidate_area(s_ui.fx_root, &cells[i]);      // clipped to the effect area
        }
    }
}

void fx_set_percent(int percent)
{
    if (!s_ui.fx_tip) {
        return;
    }
    char buf[40];
    if (percent >= 0) {
        snprintf(buf, sizeof(buf), "%s  %d%%", phone_tr("Đang sạc", "Charging"), percent);
    } else {
        snprintf(buf, sizeof(buf), "%s", phone_tr("Đang sạc", "Charging"));
    }
    if (strcmp(lv_label_get_text(s_ui.fx_tip), buf) != 0) {
        lv_label_set_text(s_ui.fx_tip, buf);
    }
}

void fx_start(int percent)
{
    if (s_ui.fx_root) {
        fx_set_percent(percent);
        return;
    }
    s_ui.rng = lv_tick_get() | 1;
    lv_obj_t *root = lv_obj_create(s_ui.root);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, FX_W, FX_H);
    lv_obj_align(root, LV_ALIGN_BOTTOM_MID, 0, -(TRACK_H + 40));
    lv_obj_remove_flag(root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    s_ui.fx_root = root;
    lv_obj_add_event_cb(root, fx_draw, LV_EVENT_DRAW_MAIN, nullptr);
    for (Particle &p : s_ui.particles) {
        fx_respawn(&p);
        p.y = FX_H - 20.0f - fx_rand01() * (FX_H * 0.75f);
        p.life = fx_rand01();
    }
    lv_obj_t *tip = lv_label_create(root);
    s_ui.fx_tip = tip;
    lv_label_set_text(tip, "");
    lv_obj_set_style_text_font(tip, phone_font_by_size(20), 0);
    lv_obj_set_style_text_color(tip, lv_color_hex(FX_BLUE_SOFT), 0);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -8);
    fx_set_percent(percent);
    s_ui.fx_tick = lv_timer_create(fx_tick, FX_TICK_MS, nullptr);
}

/* ---- Wi-Fi and battery, top right ---- */

void update_status()
{
    /* Same levels as the status bar: RSSI -60 / -72 dBm thresholds, battery in 25 % steps */
    static const lv_image_dsc_t *const wifi[] = {
        &esp_brookesia_image_middle_status_bar_wifi_close_24_24, &esp_brookesia_image_middle_status_bar_wifi_level1_24_24,
        &esp_brookesia_image_middle_status_bar_wifi_level2_24_24, &esp_brookesia_image_middle_status_bar_wifi_level3_24_24,
    };
    static const lv_image_dsc_t *const bat[] = {
        &esp_brookesia_image_middle_status_bar_battery_level1_24_24, &esp_brookesia_image_middle_status_bar_battery_level2_24_24,
        &esp_brookesia_image_middle_status_bar_battery_level3_24_24, &esp_brookesia_image_middle_status_bar_battery_level4_24_24,
        &esp_brookesia_image_middle_status_bar_battery_charge_24_24,
    };
    phone_net_status_t net = {};
    phone_net_get_status(&net);
    int level = 0;
    if (net.wifi_connected) {
        level = net.rssi >= -60 ? 3 : (net.rssi >= -72 ? 2 : 1);
    }
    lv_image_set_src(s_ui.wifi_icon, wifi[level]);

    bsp_battery_info_t b;
    if (phone_power_get_battery(&b) && b.state != BSP_BATTERY_ABSENT) {
        const int pct = LV_CLAMP(1, b.percent, 100);
        lv_label_set_text_fmt(s_ui.bat_label, "%d%%", pct);
        lv_image_set_src(s_ui.bat_icon, b.state == BSP_BATTERY_CHARGING ? bat[4] : bat[(pct - 1) / 25]);
        lv_obj_remove_flag(s_ui.bat_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.bat_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.bat_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.bat_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *status_icon(lv_obj_t *parent)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

/* ---- clock and date ---- */

/* intro: the current time is shown and each card flips onto itself, hour pair then minute pair
 * (lock screen just shown) */
void update_clock(bool animate, bool intro = false)
{
    time_t now = time(nullptr);
    struct tm tm = {};
    localtime_r(&now, &tm);
    const bool valid = tm.tm_year >= 2025 - 1900;
    char digits[5] = "0000";
    if (valid) {
        snprintf(digits, sizeof(digits), "%02d%02d", tm.tm_hour, tm.tm_min);
        static const char *const vi_wday[] = {"Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"};
        static const char *const en_wday[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static const char *const en_mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char date[64];
        if (phone_lang_get() == PHONE_LANG_VI) {
            snprintf(date, sizeof(date), "%s, %02d/%02d/%04d", vi_wday[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
        } else {
            snprintf(date, sizeof(date), "%s, %d %s %04d", en_wday[tm.tm_wday], tm.tm_mday, en_mon[tm.tm_mon], tm.tm_year + 1900);
        }
        lv_label_set_text(s_ui.date, date);
    } else {
        lv_label_set_text(s_ui.date, phone_tr("Chưa có giờ (chờ Wi-Fi)", "Time not set (waiting for Wi-Fi)"));
    }
    for (int i = 0; i < DIGITS; i++) {
        if (intro) {
            flip_set(&s_ui.digits[i], digits[i], false);
            flip_start(&s_ui.digits[i], digits[i], INTRO_DELAY_MS + (i / 2) * INTRO_STAGGER_MS, true);
        } else {
            flip_set(&s_ui.digits[i], digits[i], animate && s_ui.clock_primed);
        }
    }
    update_status();
    s_ui.clock_primed = true;

    /* the charging effect follows the battery for as long as the lock screen is on */
    bsp_battery_info_t b;
    if (phone_power_get_battery(&b) && b.state == BSP_BATTERY_CHARGING) {
        fx_start(b.percent);
    } else {
        fx_stop();
    }
}

/* ---- slide to unlock ---- */

void unlock();

void knob_set_x(int32_t x)
{
    x = LV_CLAMP(0, x, KNOB_TRAVEL);
    lv_obj_set_x(s_ui.knob, KNOB_PAD + x);
    lv_obj_set_style_opa(s_ui.hint, static_cast<lv_opa_t>(255 - 255 * x / KNOB_TRAVEL), 0);
}

void on_knob(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_area_t a;
        lv_obj_get_coords(s_ui.track, &a);
        knob_set_x(p.x - a.x1 - KNOB_PAD - KNOB_W / 2);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int32_t x = lv_obj_get_x(s_ui.knob) - KNOB_PAD;
        if (x >= KNOB_TRAVEL * 85 / 100) {
            unlock();
            return;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_ui.knob);
        lv_anim_set_values(&a, x, 0);
        lv_anim_set_duration(&a, 250);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, [](void *, int32_t v) {
            knob_set_x(v);
        });
        lv_anim_start(&a);
    }
}

void build_ui()
{
    /* System layer: above the Brookesia status bar and navigation bar */
    lv_obj_t *root = lv_obj_create(lv_layer_sys());
    s_ui.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_image_src(root, s_wall_ready ? &s_wall_dsc : &esp_brookesia_image_middle_wallpaper_dark_480_480, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);    // swallow touches meant for the app below
    lv_obj_remove_flag(root, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));

    lv_obj_t *box = lv_obj_create(root);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 24, 0);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    for (int i = 0; i < DIGITS; i++) {
        if (i) {
            lv_obj_t *gap = lv_obj_create(row);
            lv_obj_remove_style_all(gap);
            lv_obj_set_size(gap, i == 2 ? GROUP_GAP : PAIR_GAP, 1);
        }
        make_digit(row, &s_ui.digits[i]);
    }

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_RIGHT, -20, 14);
    lv_obj_remove_flag(bar, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    s_ui.wifi_icon = status_icon(bar);
    lv_obj_set_style_margin_right(s_ui.wifi_icon, 8, 0);
    s_ui.bat_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_ui.bat_label, phone_font_by_size(20), 0);
    lv_obj_set_style_text_color(s_ui.bat_label, lv_color_white(), 0);
    s_ui.bat_icon = status_icon(bar);

    s_ui.date = lv_label_create(box);
    lv_obj_set_style_text_font(s_ui.date, phone_font_by_size(24), 0);
    lv_obj_set_style_text_color(s_ui.date, lv_color_hex(SUB_COLOR), 0);

    /* iPhone 4 style track at the bottom */
    s_ui.track = lv_obj_create(root);
    lv_obj_t *t = s_ui.track;
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, TRACK_W, TRACK_H);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_set_style_radius(t, TRACK_H / 2, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_60, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(0x3A4652), 0);
    lv_obj_remove_flag(t, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    s_ui.hint = lv_label_create(t);
    lv_label_set_text(s_ui.hint, phone_tr("trượt để mở khóa", "slide to unlock"));
    lv_obj_set_style_text_font(s_ui.hint, phone_font_by_size(24), 0);
    lv_obj_set_style_text_color(s_ui.hint, lv_color_hex(0xE5E7EB), 0);
    lv_obj_align(s_ui.hint, LV_ALIGN_CENTER, KNOB_W / 2, 0);

    s_ui.knob = lv_obj_create(t);
    lv_obj_t *k = s_ui.knob;
    lv_obj_remove_style_all(k);
    lv_obj_set_size(k, KNOB_W, TRACK_H - 2 * KNOB_PAD);
    lv_obj_set_pos(k, KNOB_PAD, KNOB_PAD);
    lv_obj_set_style_radius(k, (TRACK_H - 2 * KNOB_PAD) / 2, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(0xF2F2F2), 0);
    lv_obj_set_style_bg_grad_color(k, lv_color_hex(0xA9AEB5), 0);
    lv_obj_set_style_bg_grad_dir(k, LV_GRAD_DIR_VER, 0);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(k, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_set_ext_click_area(k, 16);
    lv_obj_add_event_cb(k, on_knob, LV_EVENT_ALL, nullptr);
    lv_obj_t *arrow = lv_label_create(k);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x5A6470), 0);
    lv_obj_center(arrow);

    /* Hint "shimmer": fade in and out while the knob rests */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ui.hint);
    lv_anim_set_values(&a, 90, 255);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        if (lv_obj_get_x(s_ui.knob) <= KNOB_PAD) {
            lv_obj_set_style_text_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0);
        }
    });
    lv_anim_start(&a);

    s_ui.clock_timer = lv_timer_create([](lv_timer_t *) {
        update_clock(true);
    }, 1000, nullptr);
    lv_timer_pause(s_ui.clock_timer);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

/* The lock screen is on the system layer and covers everything, but LVGL does not know it: each redrawn area
 * drew the app or the launcher below first (the charging effect cost core 0 70-88 %). What is below is made
 * fully transparent while locked, which LVGL skips (lv_obj_refr). Not the HIDDEN flag: clearing it on a screen
 * or a layer (no parent) crashes in lv_obj_mark_layout_as_dirty(NULL). Only what this changed is restored, and
 * only if it still exists. */
lv_obj_t *s_below[3];

void below_hide(bool hide)
{
    if (hide) {
        lv_obj_t *const layers[3] = {lv_screen_active(), lv_layer_top(), lv_layer_bottom()};
        for (int i = 0; i < 3; i++) {
            s_below[i] = nullptr;
            if (layers[i] && lv_obj_get_style_opa_layered(layers[i], LV_PART_MAIN) > LV_OPA_MIN) {
                lv_obj_set_style_opa_layered(layers[i], LV_OPA_TRANSP, LV_PART_MAIN);
                s_below[i] = layers[i];
            }
        }
        return;
    }
    for (lv_obj_t *&o : s_below) {
        if (o && lv_obj_is_valid(o)) {
            lv_obj_remove_local_style_prop(o, LV_STYLE_OPA_LAYERED, LV_PART_MAIN);
        }
        o = nullptr;
    }
}

/* LVGL context. intro: play the flip-in animation (the screen is about to be seen) */
void lock_show(bool intro)
{
    if (!s_ui.root) {
        build_ui();
    }
    if (s_wall_ready && lv_obj_get_style_bg_image_src(s_ui.root, LV_PART_MAIN) != &s_wall_dsc) {
        lv_obj_set_style_bg_image_src(s_ui.root, &s_wall_dsc, 0);
    }
    if (lv_obj_has_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN)) {
        s_ui.clock_primed = false;
        knob_set_x(0);
        lv_obj_remove_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.root);
        below_hide(true);
    }
    update_clock(false, intro);
    lv_timer_resume(s_ui.clock_timer);
    lv_display_trigger_activity(nullptr);
}

/* LVGL context */
void unlock()
{
    fx_stop();
    lv_timer_pause(s_ui.clock_timer);
    below_hide(false);
    lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_HIDDEN);
    s_state = ST_ACTIVE;
    lv_display_trigger_activity(nullptr);
}

/* ---------------- state changes ---------------- */

/* Worker task */
void go_off()
{
    if (s_state == ST_OFF) {
        return;
    }
    bsp_display_brightness_set(0);
    s_state = ST_OFF;
    wifi_power_save(true);
    /* nothing runs for the dark screen: the effect is removed (the clock tick starts it again on wake) */
    if (bsp_display_lock(1000)) {
        fx_stop();
        bsp_display_unlock();
    }
    /* Nothing to draw with the backlight off: stop LVGL and its tick */
    s_paused = esp_lv_adapter_pause(1000) == ESP_OK;
    ESP_LOGI(TAG, "Screen off");
}

/* Worker task: screen back on, showing the lock screen */
void wake_to_lock()
{
    if (s_paused) {
        esp_lv_adapter_resume();
        s_paused = false;
    }
    wifi_power_save(false);
    if (bsp_display_lock(1000)) {
        lock_show(true);
        bsp_display_unlock();
    }
    s_state = ST_LOCKED;
    vTaskDelay(pdMS_TO_TICKS(FIRST_FRAME_MS));
    bsp_display_brightness_set(user_brightness());
    ESP_LOGI(TAG, "Screen on (locked)");
}

/* Worker task */
void on_key()
{
    switch (s_state.load()) {
    case ST_OFF:
        wake_to_lock();
        break;
    case ST_LOCKED:
        go_off();
        break;
    default:   // in use or dimmed: show the lock screen
        if (bsp_display_lock(1000)) {
            lock_show(true);
            bsp_display_unlock();
        }
        s_state = ST_LOCKED;
        bsp_display_brightness_set(user_brightness());
        break;
    }
}

void worker(void *)
{
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        if (bits & NB_OFF) {
            go_off();
        }
        if (bits & NB_KEY) {
            on_key();
        }
        if (bits & NB_WAKE) {
            if (s_state == ST_OFF) {
                wake_to_lock();
            }
            xSemaphoreGive(s_wake_done);
        }
    }
}

/* LVGL timer: dim and switch off on inactivity */
void idle_tick(lv_timer_t *)
{
    const uint32_t idle = lv_display_get_inactive_time(nullptr);
    const uint32_t to = timeout_ms();
    switch (s_state.load()) {
    case ST_ACTIVE:
        if (to && idle >= to - LV_MIN(DIM_LEAD_MAX_MS, to / 2)) {
            bsp_display_brightness_set(DIM_PERCENT);
            s_state = ST_DIM;
        }
        break;
    case ST_DIM:
        if (!to || idle < s_last_idle) {
            /* touched (or timeout set to never): back to full brightness, the touch goes on to the app */
            bsp_display_brightness_set(user_brightness());
            s_state = ST_ACTIVE;
        } else if (idle >= to) {
            lock_show(false);    // the screen goes off right away
            s_state = ST_LOCKED;
            xTaskNotify(s_worker, NB_OFF, eSetBits);
        }
        break;
    case ST_LOCKED:
        if (idle >= LOCK_IDLE_MS) {
            xTaskNotify(s_worker, NB_OFF, eSetBits);
        }
        break;
    default:
        break;
    }
    s_last_idle = lv_display_get_inactive_time(nullptr);
}

} // namespace

extern "C" void phone_lock_start(void)
{
    if (s_worker) {
        return;
    }
    s_wake_done = xSemaphoreCreateBinary();
    if (xTaskCreate(worker, "lock", 3072, nullptr, 4, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "Lock task create failed");
        return;
    }
    lv_timer_create(idle_tick, IDLE_TICK_MS, nullptr);
    /* Low priority: ready long before the first lock screen */
    xTaskCreate(blur_task, "wall_blur", 3072, nullptr, 1, nullptr);
}

extern "C" void phone_lock_on_power_key(void)
{
    if (s_worker) {
        xTaskNotify(s_worker, NB_KEY, eSetBits);
    }
}

extern "C" void phone_lock_wake(void)
{
    if (!s_worker) {
        return;
    }
    if (s_state == ST_OFF) {
        xSemaphoreTake(s_wake_done, 0);
        xTaskNotify(s_worker, NB_WAKE, eSetBits);
        xSemaphoreTake(s_wake_done, pdMS_TO_TICKS(2000));
    } else if (s_state == ST_DIM) {
        bsp_display_brightness_set(user_brightness());
        s_state = ST_ACTIVE;
    }
}

extern "C" void phone_lock_on_charger(void)
{
    if (s_worker && s_state == ST_OFF) {
        xTaskNotify(s_worker, NB_WAKE, eSetBits);    // the lock screen shows the charging effect
    }
}

extern "C" int phone_lock_timeout_count(void)
{
    return sizeof(TIMEOUTS_S) / sizeof(TIMEOUTS_S[0]);
}

extern "C" const char *phone_lock_timeout_label(int index)
{
    static const char *const vi[] = {"15 giây", "30 giây", "1 phút", "2 phút", "5 phút", "Không bao giờ"};
    static const char *const en[] = {"15 seconds", "30 seconds", "1 minute", "2 minutes", "5 minutes", "Never"};
    if (index < 0 || index >= phone_lock_timeout_count()) {
        return "";
    }
    return phone_lang_get() == PHONE_LANG_VI ? vi[index] : en[index];
}

extern "C" bool phone_lock_is_locked(void)
{
    const int st = s_state.load();
    return st == ST_LOCKED || st == ST_OFF;
}

extern "C" int phone_lock_timeout_get(void)
{
    int i = phone_pref_get("scr_to", TIMEOUT_DEFAULT);
    return (i >= 0 && i < phone_lock_timeout_count()) ? i : TIMEOUT_DEFAULT;
}

extern "C" void phone_lock_timeout_set(int index)
{
    if (index >= 0 && index < phone_lock_timeout_count()) {
        phone_pref_set("scr_to", index);
    }
}
