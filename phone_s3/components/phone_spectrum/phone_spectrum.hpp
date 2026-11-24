#pragma once

#include <cstdint>
#include "lvgl.h"

/**
 * Spectrum of the sound that plays, shared by the Radio and Music apps: the player feeds its mono
 * 16-bit PCM, a low-priority task (started on the first feed, PSRAM stack) runs a 256-point FFT
 * about 30 times a second and keeps 12 log bands from 60 Hz to 6 kHz. Without new samples for a
 * moment the bands fall to zero. The view draws them as rainbow bars (the look of the Radio app).
 */
namespace spectrum {

constexpr int BAR_COUNT = 12;

/** Mono samples at `rate` Hz, from the player task; copies into a ring, no FFT here. */
void feed(const int16_t *mono, int count, int rate);
/** Current levels, 0..255 per band (any task). */
void bars(uint8_t out[BAR_COUNT]);

/** Bars in `box`, which the caller sizes, colors and pads (they fill its content area). */
struct View {
    lv_obj_t *box;
    lv_obj_t *bar[BAR_COUNT];
    int gap;
    int min_h;
};
void view_create(View &view, lv_obj_t *box, int gap, int min_h, int radius);
/** LVGL task: set the bars from the current levels (only those whose height changed). */
void view_update(View &view);
/** LVGL task: all bars at their minimum height. */
void view_clear(View &view);

} // namespace spectrum
