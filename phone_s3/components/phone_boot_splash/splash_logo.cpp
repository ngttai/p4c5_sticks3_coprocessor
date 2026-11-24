#include "phone_boot_splash.h"
#include <cstdint>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

extern const uint8_t splash_bin_start[] asm("_binary_splash_bin_start");
extern const uint8_t splash_bin_end[] asm("_binary_splash_bin_end");

namespace {

const char *TAG = "splash";
constexpr int LOGO = 480;               // the frames are cut from a 480 x 480 logo, centered on the screen
constexpr uint32_t HOLD_MS = 500;       // the last frame stays a moment
constexpr uint32_t FADE_MS = 300;

struct Header {
    uint16_t frames, width, height, x, y, frame_ms;
    uint8_t bg[3];
    const uint8_t *data;                // first QOI file
    const uint8_t *offsets;             // u32 per frame + 1, unaligned
};

Header s_hdr;
lv_obj_t *s_root;
lv_obj_t *s_img;
lv_timer_t *s_timer;
uint16_t *s_buf[2];
lv_image_dsc_t s_dsc[2];
int s_frame;
int s_front;
bool s_ready;
bool s_done;                            // the animation has played to the end
uint32_t s_done_at;

uint16_t rd16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool parse()
{
    const uint8_t *p = splash_bin_start;
    const size_t len = splash_bin_end - splash_bin_start;
    if (len < 22 || memcmp(p, "KSPL", 4) != 0 || rd16(p + 4) != 1) {
        return false;
    }
    s_hdr.frames = rd16(p + 6);
    s_hdr.width = rd16(p + 8);
    s_hdr.height = rd16(p + 10);
    s_hdr.x = rd16(p + 12);
    s_hdr.y = rd16(p + 14);
    s_hdr.frame_ms = rd16(p + 16);
    memcpy(s_hdr.bg, p + 18, 3);
    s_hdr.offsets = p + 22;
    s_hdr.data = s_hdr.offsets + 4 * (s_hdr.frames + 1);
    return s_hdr.frames > 0 && s_hdr.data + rd32(s_hdr.offsets + 4 * s_hdr.frames) <= splash_bin_end;
}

/* QOI (https://qoiformat.org) straight into RGB565, no intermediate RGB buffer */
bool decode(int frame, uint16_t *out)
{
    const uint8_t *p = s_hdr.data + rd32(s_hdr.offsets + 4 * frame);
    const uint8_t *end = s_hdr.data + rd32(s_hdr.offsets + 4 * (frame + 1));
    if (end - p < 22 || memcmp(p, "qoif", 4) != 0) {
        return false;
    }
    const uint32_t w = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
    const uint32_t h = (p[8] << 24) | (p[9] << 16) | (p[10] << 8) | p[11];
    if (w != s_hdr.width || h != s_hdr.height) {
        return false;
    }
    p += 14;
    end -= 8;                           // end marker
    uint8_t index[64][4] = {};
    uint8_t r = 0, g = 0, b = 0, a = 255;
    int run = 0;
    const size_t count = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < count; i++) {
        if (run > 0) {
            run--;
        } else if (p < end) {
            const uint8_t op = *p++;
            if (op == 0xfe) {
                r = p[0];
                g = p[1];
                b = p[2];
                p += 3;
            } else if (op == 0xff) {
                r = p[0];
                g = p[1];
                b = p[2];
                a = p[3];
                p += 4;
            } else if ((op & 0xc0) == 0x00) {
                r = index[op][0];
                g = index[op][1];
                b = index[op][2];
                a = index[op][3];
            } else if ((op & 0xc0) == 0x40) {
                r += ((op >> 4) & 3) - 2;
                g += ((op >> 2) & 3) - 2;
                b += (op & 3) - 2;
            } else if ((op & 0xc0) == 0x80) {
                const int dg = (op & 0x3f) - 32;
                const uint8_t d = *p++;
                r += dg - 8 + ((d >> 4) & 0x0f);
                g += dg;
                b += dg - 8 + (d & 0x0f);
            } else {
                run = op & 0x3f;
            }
            uint8_t *e = index[(r * 3 + g * 5 + b * 7 + a * 11) % 64];
            e[0] = r;
            e[1] = g;
            e[2] = b;
            e[3] = a;
        }
        out[i] = static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    }
    return true;
}

void finish(lv_anim_t *)
{
    lv_timer_delete(s_timer);
    s_timer = nullptr;
    lv_obj_delete(s_root);              // the image goes with it before its buffers
    s_root = nullptr;
    for (int i = 0; i < 2; i++) {
        lv_image_cache_drop(&s_dsc[i]);
        heap_caps_free(s_buf[i]);
        s_buf[i] = nullptr;
    }
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
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_completed_cb(&a, finish);
    lv_anim_start(&a);
}

/* The first frame stays until the phone is ready: the boot keeps the LVGL lock for about a second
 * and an animation started earlier stopped half way. Then one frame per call, not by the clock. */
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
    if (!s_done) {
        if (s_frame + 1 < s_hdr.frames) {
            const int back = s_front ^ 1;
            if (decode(s_frame + 1, s_buf[back])) {
                lv_image_cache_drop(&s_dsc[back]);
                lv_image_set_src(s_img, &s_dsc[back]);
                s_front = back;
            }
            s_frame++;
        }
        if (s_frame + 1 >= s_hdr.frames) {
            s_done = true;
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
    if (s_root || !parse()) {
        ESP_LOGW(TAG, "no splash");
        return;
    }
    const size_t size = static_cast<size_t>(s_hdr.width) * s_hdr.height * 2;
    for (int i = 0; i < 2; i++) {
        s_buf[i] = static_cast<uint16_t *>(heap_caps_aligned_alloc(64, size, MALLOC_CAP_SPIRAM));
        if (!s_buf[i]) {
            heap_caps_free(s_buf[0]);
            s_buf[0] = nullptr;
            ESP_LOGW(TAG, "no memory");
            return;
        }
        s_dsc[i] = {};
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w = s_hdr.width;
        s_dsc[i].header.h = s_hdr.height;
        s_dsc[i].header.stride = s_hdr.width * 2;
        s_dsc[i].data = reinterpret_cast<const uint8_t *>(s_buf[i]);
        s_dsc[i].data_size = size;
    }
    if (!decode(0, s_buf[0])) {
        ESP_LOGW(TAG, "bad frame");
    }

    s_root = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_make(s_hdr.bg[0], s_hdr.bg[1], s_hdr.bg[2]), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);         // nothing under it takes a touch
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(s_root);
    s_img = lv_image_create(s_root);
    lv_image_set_src(s_img, &s_dsc[0]);
    lv_obj_set_pos(s_img, (lv_obj_get_width(s_root) - LOGO) / 2 + s_hdr.x, (lv_obj_get_height(s_root) - LOGO) / 2 + s_hdr.y);
    s_frame = 0;
    s_front = 0;
    s_timer = lv_timer_create(on_timer, s_hdr.frame_ms, nullptr);
    ESP_LOGI(TAG, "%d frames %dx%d", s_hdr.frames, s_hdr.width, s_hdr.height);
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
