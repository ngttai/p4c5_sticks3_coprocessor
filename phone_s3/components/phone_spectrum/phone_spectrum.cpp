/*
 * Spectrum of the playing sound (moved from the Radio app, now shared with Music): a ring of mono
 * samples fed by the player, a 256-point Q15 FFT (gmf_fft) in a low-priority task, 12 log bands
 * from 60 Hz to 6 kHz with attack/release smoothing, and rainbow bars for LVGL.
 */
#include "phone_spectrum.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "esp_gmf_fft.h"
#include "esp_gmf_fft_heap.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

namespace spectrum {

namespace {

const char *TAG = "spectrum";
constexpr int FFT_N = 256;
constexpr uint32_t RING = 2048;             // mono samples, power of 2
constexpr float ATTACK = 0.55f;
constexpr float RELEASE = 0.18f;
constexpr int64_t IDLE_US = 120 * 1000;     // no samples for this long: the bars fall
/* Levels in dB. Music loses about 3 dB per octave going up, so each band (0.55 octave) gets +1.7 dB:
 * without it the bass band was always the highest. The reference is the loudest band, falling back
 * slowly (4.5 dB/s) after a peak; a band at the reference fills 92 % of the height, RANGE_DB below
 * it is empty. Before: each frame was divided by its own loudest band, which was then always full. */
constexpr float TILT_DB = 1.7f;
constexpr float RANGE_DB = 36.f;
constexpr float REF_FALL_DB = 0.15f;        // per frame (about 30 a second)
constexpr float REF_FLOOR_DB = 34.f;        // quiet passages are not blown up to full bars
constexpr float TOP = 0.92f;

constexpr uint32_t RAINBOW[BAR_COUNT] = {
    0xFF3B5C, 0xFF6B2D, 0xFFB020, 0xFFE84A, 0x7CFF4A, 0x2DFFB0,
    0x2DE8FF, 0x3D8BFF, 0x6B5CFF, 0xB24DFF, 0xFF4DC8, 0xFF4D7A,
};

alignas(16) int16_t s_ring[RING];
std::atomic<uint32_t> s_ring_w;
std::atomic<uint32_t> s_ring_r;
std::atomic<int> s_rate;
std::atomic<int64_t> s_last_feed;
std::atomic<bool> s_task_started;
portMUX_TYPE s_bar_mux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_bars[BAR_COUNT];

bool ring_pop(int16_t *out, int n)
{
    const uint32_t w = s_ring_w.load(std::memory_order_acquire);
    uint32_t r = s_ring_r.load(std::memory_order_relaxed);
    if (w - r < static_cast<uint32_t>(n)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        out[i] = s_ring[r++ & (RING - 1)];
    }
    s_ring_r.store(r, std::memory_order_release);
    return true;
}

void publish(const float *levels)
{
    portENTER_CRITICAL(&s_bar_mux);
    for (int i = 0; i < BAR_COUNT; i++) {
        s_bars[i] = static_cast<uint8_t>(std::clamp(levels[i], 0.f, 1.f) * 255.f + 0.5f);
    }
    portEXIT_CRITICAL(&s_bar_mux);
}

uint32_t isqrt(uint32_t n)
{
    if (n == 0) {
        return 0;
    }
    uint32_t x = n, y = (n + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

uint32_t magnitude(const int16_t *d, int k)
{
    int32_t re, im = 0;
    if (k == 0) {
        re = d[0];
    } else if (k == FFT_N / 2) {
        re = d[1];
    } else {
        re = d[2 * k];
        im = d[2 * k + 1];
    }
    return isqrt(static_cast<uint32_t>(re * re) + static_cast<uint32_t>(im * im));
}

uint32_t mix(uint32_t a, uint32_t b, float t)
{
    auto ch = [&](int s) {
        const float x = ((a >> s) & 0xFF) + (((b >> s) & 0xFF) - static_cast<float>((a >> s) & 0xFF)) * t;
        return static_cast<uint32_t>(x) & 0xFF;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

/* Runs for good once started: about 30 FFTs a second while samples come, idle otherwise */
void fft_task(void *)
{
    esp_gmf_fft_handle_t fft = nullptr;
    const esp_gmf_fft_cfg_t cfg = {.n_fft = FFT_N, .fft_type = ESP_GMF_FFT_TYPE_REAL_Q15};
    auto *buf = static_cast<int16_t *>(esp_gmf_fft_calloc_aligned(ESP_GMF_FFT_BUFFER_SIZE(FFT_N), sizeof(int16_t), 16));
    if (!buf || esp_gmf_fft_init(&cfg, &fft) != ESP_GMF_FFT_OK) {
        ESP_LOGE(TAG, "fft init failed");
        esp_gmf_fft_free_aligned(buf);
        s_task_started = false;
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    static int16_t hann[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        hann[i] = static_cast<int16_t>(0.5f * (1.f - cosf(2.f * static_cast<float>(M_PI) * i / (FFT_N - 1))) * 32767.f);
    }
    int lo[BAR_COUNT], hi[BAR_COUNT];
    int bands_rate = 0;
    float smooth[BAR_COUNT] = {};
    bool idle_zero = true;
    float ref_db = REF_FLOOR_DB;
    static int16_t pcm[FFT_N];
    for (;;) {
        const int rate = s_rate;
        if (rate <= 0 || esp_timer_get_time() - s_last_feed > IDLE_US) {
            if (!idle_zero) {
                bool any = false;
                for (float &v : smooth) {
                    v *= 1.f - RELEASE;
                    if (v < 0.004f) {
                        v = 0;
                    }
                    any = any || v > 0;
                }
                publish(smooth);
                idle_zero = !any;
            }
            vTaskDelay(pdMS_TO_TICKS(idle_zero ? 100 : 40));
            continue;
        }
        if (idle_zero) {
            ref_db = REF_FLOOR_DB;              /* a new sound: find its level again */
        }
        idle_zero = false;
        if (rate != bands_rate) {
            bands_rate = rate;
            const float bin_hz = static_cast<float>(rate) / FFT_N;
            for (int b = 0; b < BAR_COUNT; b++) {
                const float f0 = 60.f * powf(100.f, static_cast<float>(b) / BAR_COUNT);
                const float f1 = 60.f * powf(100.f, static_cast<float>(b + 1) / BAR_COUNT);
                lo[b] = std::max(1, static_cast<int>(f0 / bin_hz));
                hi[b] = std::max(lo[b], std::min(FFT_N / 2 - 1, static_cast<int>(f1 / bin_hz)));
            }
        }
        if (!ring_pop(pcm, FFT_N)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        for (int i = 0; i < FFT_N; i++) {
            buf[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * hann[i]) >> 15);
        }
        for (size_t i = FFT_N; i < ESP_GMF_FFT_BUFFER_SIZE(FFT_N); i++) {
            buf[i] = 0;
        }
        if (esp_gmf_fft_forward(fft, buf) != ESP_GMF_FFT_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        float db[BAR_COUNT];
        float loudest = -100.f;
        for (int b = 0; b < BAR_COUNT; b++) {
            uint32_t sum = 0;
            for (int k = lo[b]; k <= hi[b]; k++) {
                sum += magnitude(buf, k);
            }
            const float raw = static_cast<float>(sum) / (hi[b] - lo[b] + 1);
            db[b] = 20.f * log10f(raw + 1.f) + TILT_DB * b;
            loudest = std::max(loudest, db[b]);
        }
        ref_db = std::max({ref_db - REF_FALL_DB, loudest, REF_FLOOR_DB});
        for (int b = 0; b < BAR_COUNT; b++) {
            const float n = TOP * std::clamp((db[b] - (ref_db - RANGE_DB)) / RANGE_DB, 0.f, 1.f);
            smooth[b] += (n - smooth[b]) * (n > smooth[b] ? ATTACK : RELEASE);
        }
        publish(smooth);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void start_task()
{
    if (s_task_started.exchange(true)) {
        return;
    }
    TaskHandle_t t = nullptr;
    if (xTaskCreatePinnedToCoreWithCaps(fft_task, "spectrum", 6144, nullptr, 3, &t, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "task not created");
        s_task_started = false;
    }
}

} // namespace

void feed(const int16_t *mono, int count, int rate)
{
    if (!mono || count <= 0 || rate <= 0) {
        return;
    }
    start_task();
    if (rate != s_rate) {
        s_rate = rate;
        s_ring_r.store(s_ring_w.load());    /* drop samples of another rate */
    }
    uint32_t w = s_ring_w.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        s_ring[w++ & (RING - 1)] = mono[i];
    }
    s_ring_w.store(w, std::memory_order_release);
    const uint32_t r = s_ring_r.load(std::memory_order_relaxed);
    if (w - r > RING) {
        s_ring_r.store(w - RING, std::memory_order_relaxed);
    }
    s_last_feed = esp_timer_get_time();
}

void bars(uint8_t out[BAR_COUNT])
{
    portENTER_CRITICAL(&s_bar_mux);
    memcpy(out, s_bars, BAR_COUNT);
    portEXIT_CRITICAL(&s_bar_mux);
}

void view_create(View &view, lv_obj_t *box, int gap, int min_h, int radius)
{
    view.box = box;
    view.gap = gap;
    view.min_h = min_h;
    for (auto &bar : view.bar) {
        bar = lv_obj_create(box);
        lv_obj_remove_style_all(bar);
        lv_obj_remove_flag(bar, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, radius, 0);
        lv_obj_set_height(bar, 0);
    }
    lv_obj_update_layout(box);
    view_clear(view);
}

void view_update(View &view)
{
    uint8_t lv[BAR_COUNT];
    bars(lv);
    const int w = lv_obj_get_content_width(view.box);
    const int h = lv_obj_get_content_height(view.box);
    const int bar_w = (w - (BAR_COUNT - 1) * view.gap) / BAR_COUNT;
    const int x0 = (w - (BAR_COUNT * bar_w + (BAR_COUNT - 1) * view.gap)) / 2;
    for (int i = 0; i < BAR_COUNT; i++) {
        const float t = lv[i] / 255.f;
        const int bh = view.min_h + static_cast<int>((h - view.min_h) * t);
        if (lv_obj_get_height(view.bar[i]) == bh && lv_obj_get_width(view.bar[i]) == bar_w) {
            continue;
        }
        lv_obj_set_size(view.bar[i], bar_w, bh);
        lv_obj_set_pos(view.bar[i], x0 + i * (bar_w + view.gap), h - bh);
        const uint32_t base = mix(RAINBOW[i], 0xFFFFFF, 0.35f * t);
        lv_obj_set_style_bg_color(view.bar[i], lv_color_hex(mix(0x2A2F3A, base, 0.25f + 0.75f * t)), 0);
    }
}

void view_clear(View &view)
{
    const int w = lv_obj_get_content_width(view.box);
    const int h = lv_obj_get_content_height(view.box);
    const int bar_w = (w - (BAR_COUNT - 1) * view.gap) / BAR_COUNT;
    const int x0 = (w - (BAR_COUNT * bar_w + (BAR_COUNT - 1) * view.gap)) / 2;
    for (int i = 0; i < BAR_COUNT; i++) {
        lv_obj_set_size(view.bar[i], bar_w, view.min_h);
        lv_obj_set_pos(view.bar[i], x0 + i * (bar_w + view.gap), h - view.min_h);
        lv_obj_set_style_bg_color(view.bar[i], lv_color_hex(mix(0x2A2F3A, RAINBOW[i], 0.25f)), 0);
    }
}

} // namespace spectrum
