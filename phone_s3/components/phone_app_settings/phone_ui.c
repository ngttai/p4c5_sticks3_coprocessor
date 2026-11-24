#include "phone_ui.h"

lv_obj_t *phone_ui_mute_icon(lv_obj_t *parent, int size, lv_color_t color)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &phone_icon_mute);
    lv_image_set_scale(img, (uint32_t)(size * LV_SCALE_NONE / 48));
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(img, size, size);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

void phone_ui_scrollbar(lv_obj_t *obj)
{
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(obj, 8, LV_PART_SCROLLBAR);
}
