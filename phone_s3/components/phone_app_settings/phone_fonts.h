#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(phone_font_12)
LV_FONT_DECLARE(phone_font_14)
LV_FONT_DECLARE(phone_font_16)
LV_FONT_DECLARE(phone_font_18)
LV_FONT_DECLARE(phone_font_20)
LV_FONT_DECLARE(phone_font_24)
LV_FONT_DECLARE(phone_font_28)
/** 20 px Chinese (3755 common hanzi and punctuation), falls back to phone_font_20. */
LV_FONT_DECLARE(phone_font_cjk_20)

/** Vietnamese-capable UI font closest to size_px (12 to 28 px). */
const lv_font_t *phone_font_by_size(int size_px);

#ifdef __cplusplus
}
#endif
