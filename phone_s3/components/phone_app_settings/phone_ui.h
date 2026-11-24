#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The scrollbar of the Settings app on a scrolling list: a thin bar on the right, shown only while the
 * list moves (objects made with lv_obj_remove_style_all() have none). Call with the LVGL lock held.
 */
void phone_ui_scrollbar(lv_obj_t *obj);

/** The crossed speaker of every mute indicator (48 px image, ESP-Brookesia, as in esp_chatbot_v1). */
extern const lv_image_dsc_t phone_icon_mute;

/** The mute icon, `size` px square in `color` (an image: color it later with image_recolor). */
lv_obj_t *phone_ui_mute_icon(lv_obj_t *parent, int size, lv_color_t color);

#ifdef __cplusplus
}
#endif
