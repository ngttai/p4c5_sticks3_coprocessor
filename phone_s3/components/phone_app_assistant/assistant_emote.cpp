#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "phone_paths.h"
#include "phone_storage.h"
#include "assistant_emote.h"
extern "C" {
#include "gfx_eaf_dec.h"
}

namespace {

const char *TAG = "assistant_emote";

#define EMOTE_ASSET(name) \
    extern const uint8_t name##_start[] asm("_binary_pack_" #name "_eaf_start"); \
    extern const uint8_t name##_end[] asm("_binary_pack_" #name "_eaf_end");

EMOTE_ASSET(smile_static)
EMOTE_ASSET(idle)
EMOTE_ASSET(happy)
EMOTE_ASSET(laugh)
EMOTE_ASSET(snigger)
EMOTE_ASSET(loving)
EMOTE_ASSET(confident)
EMOTE_ASSET(yummy)
EMOTE_ASSET(silly)
EMOTE_ASSET(winking)
EMOTE_ASSET(relaxed)
EMOTE_ASSET(leisure)
EMOTE_ASSET(shy)
EMOTE_ASSET(ponder)
EMOTE_ASSET(confused)
EMOTE_ASSET(sad)
EMOTE_ASSET(cry)
EMOTE_ASSET(angry)
EMOTE_ASSET(surprised)
EMOTE_ASSET(shocked)
EMOTE_ASSET(asleep)

#define PACK(name) #name, name##_start, name##_end

/* Emotion names of the Xiaozhi protocol -> animation of Espressif's Emote Pack (eaf_360x360), embedded */
const struct {
    const char *emotion;
    const char *pack;           // the name in the pack: pack_<name>.eaf
    const uint8_t *start;
    const uint8_t *end;
    int fps;
} MAP[] = {
    {"neutral", PACK(smile_static), 20}, {"idle", PACK(idle), 20},
    {"happy", PACK(happy), 20}, {"laughing", PACK(laugh), 20}, {"funny", PACK(snigger), 20},
    {"loving", PACK(loving), 20}, {"kissy", PACK(loving), 20}, {"confident", PACK(confident), 20},
    {"delicious", PACK(yummy), 20}, {"silly", PACK(silly), 20}, {"winking", PACK(winking), 20},
    {"relaxed", PACK(relaxed), 20}, {"cool", PACK(leisure), 20}, {"embarrassed", PACK(shy), 20},
    {"thinking", PACK(ponder), 20}, {"confused", PACK(confused), 20},
    {"sad", PACK(sad), 20}, {"crying", PACK(cry), 20}, {"angry", PACK(angry), 20},
    {"surprised", PACK(surprised), 20}, {"shocked", PACK(shocked), 20}, {"sleepy", PACK(asleep), 5},
};

/* A card can bring its own faces: appdata/assistant/emote/<emotion>.eaf, else pack_<name>.eaf (a folder of the
 * Emote Pack copied as it is); any size up to MAX_SIDE. Bigger than SIZE: each frame is decoded aside and shrunk
 * into SIZE once (nearest pixel), so LVGL copies the image as it is; scaling it in LVGL is done in software on every
 * redraw (about 80% of core 0 for a 400x400 set). Smaller ones are left to LVGL to enlarge. */
constexpr const char *SD_DIR = PHONE_SD_ASSISTANT_EMOTE;
constexpr int MAX_SIDE = 480;
constexpr size_t MAX_FILE = 4 * 1024 * 1024;

constexpr int SIZE = ASSISTANT_EMOTE_SIZE;

lv_obj_t *s_img;
lv_timer_t *s_timer;
lv_image_dsc_t s_dsc;
uint16_t *s_frame;              // the image shown
size_t s_frame_px;              // pixels s_frame holds
int s_w, s_h;                   // size of the animation playing (frames as decoded)
int s_ow, s_oh;                 // size of s_frame: s_w x s_h, or shrunk to fit SIZE
uint16_t *s_src;                // a frame decoded before it is shrunk (PSRAM), nullptr when not shrunk
uint16_t s_sx[SIZE];            // source column of each column shown, when shrunk
uint16_t s_background;
eaf_dec_handle_t s_dec;
uint8_t *s_file;                // the animation read from the card (PSRAM), or nullptr when embedded
bool s_from_card;               // the animation playing comes from the card
char s_emotion[32];
int s_index, s_total;

uint16_t rgb565(uint32_t hex)
{
    return static_cast<uint16_t>(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}

/* Decode the next valid frame; the flat color around the eyes becomes the app background */
void show_next()
{
    if (!s_dec || !s_frame || s_total <= 0) {
        return;
    }
    for (int tries = 0; tries < s_total; tries++) {
        const int index = s_index;
        s_index = (s_index + 1) % s_total;
        eaf_dec_header_t header;
        if (eaf_dec_get_frame_info(s_dec, index, &header) != EAF_DEC_TYPE_VALID) {
            continue;       /* the pack ends with a marker frame */
        }
        const bool fits = header.width == s_w && header.height == s_h;
        eaf_dec_free_header(&header);
        uint16_t *out = s_src ? s_src : s_frame;
        if (!fits || eaf_dec_decode_frame(s_dec, index, reinterpret_cast<uint8_t *>(out),
                                          static_cast<size_t>(s_w) * s_h * 2, false) != ESP_OK) {
            continue;
        }
        const uint16_t key = out[0];
        if (s_src) {
            /* shrink and put the background in one pass */
            uint16_t *dst = s_frame;
            for (int y = 0; y < s_oh; y++) {
                const uint16_t *row = s_src + static_cast<size_t>(y * s_h / s_oh) * s_w;
                for (int x = 0; x < s_ow; x++) {
                    const uint16_t c = row[s_sx[x]];
                    *dst++ = c == key ? s_background : c;
                }
            }
        } else if (key != s_background) {
            for (int i = 0; i < s_w * s_h; i++) {
                if (s_frame[i] == key) {
                    s_frame[i] = s_background;
                }
            }
        }
        lv_obj_invalidate(s_img);
        return;
    }
}

void on_timer(lv_timer_t *)
{
    show_next();
}

/* The whole file in PSRAM, or nullptr when it is not there */
uint8_t *read_card(const char *name, size_t *size)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.eaf", SD_DIR, name);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > MAX_FILE) {
        return nullptr;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return nullptr;
    }
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM));
    const bool ok = buf && fread(buf, 1, st.st_size, f) == static_cast<size_t>(st.st_size);
    fclose(f);
    if (!ok) {
        heap_caps_free(buf);
        return nullptr;
    }
    *size = st.st_size;
    ESP_LOGI(TAG, "%s from the card (%u bytes)", path, static_cast<unsigned>(*size));
    return buf;
}

bool card_mounted()
{
    uint64_t total = 0, free_bytes = 0;
    return phone_storage_get(&total, &free_bytes) == PHONE_STORAGE_MOUNTED;
}

void close_animation()
{
    if (s_dec) {
        eaf_dec_deinit(s_dec);
        s_dec = nullptr;
    }
    heap_caps_free(s_file);
    s_file = nullptr;
    heap_caps_free(s_src);
    s_src = nullptr;
    s_total = 0;
}

/* Open data as the animation playing; false when it cannot be decoded or is too big */
bool open_animation(const uint8_t *data, size_t size)
{
    if (eaf_dec_init(data, size, &s_dec) != ESP_OK) {
        s_dec = nullptr;
        return false;
    }
    s_total = eaf_dec_get_total_frames(s_dec);
    int w = 0, h = 0;
    for (int i = 0; i < s_total && !w; i++) {
        eaf_dec_header_t header;
        if (eaf_dec_get_frame_info(s_dec, i, &header) == EAF_DEC_TYPE_VALID) {
            w = header.width;
            h = header.height;
            eaf_dec_free_header(&header);
        }
    }
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE) {
        ESP_LOGW(TAG, "animation of %dx%d not shown", w, h);
        eaf_dec_deinit(s_dec);
        s_dec = nullptr;
        return false;
    }
    /* shrunk to SIZE on the longer side when bigger */
    const int side = w > h ? w : h;
    const int ow = side > SIZE ? w * SIZE / side : w, oh = side > SIZE ? h * SIZE / side : h;
    if (side > SIZE) {
        s_src = static_cast<uint16_t *>(heap_caps_malloc(static_cast<size_t>(w) * h * 2, MALLOC_CAP_SPIRAM));
        if (!s_src) {
            eaf_dec_deinit(s_dec);
            s_dec = nullptr;
            return false;
        }
        for (int x = 0; x < ow; x++) {
            s_sx[x] = static_cast<uint16_t>(x * w / ow);
        }
    }
    const size_t px = static_cast<size_t>(ow) * oh;
    if (px > s_frame_px) {
        auto *frame = static_cast<uint16_t *>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM));
        if (!frame) {
            eaf_dec_deinit(s_dec);
            s_dec = nullptr;
            heap_caps_free(s_src);
            s_src = nullptr;
            return false;
        }
        heap_caps_free(s_frame);
        s_frame = frame;
        s_frame_px = px;
    }
    s_w = w;
    s_h = h;
    if (ow != s_ow || oh != s_oh || s_dsc.data != reinterpret_cast<const uint8_t *>(s_frame)) {
        s_ow = ow;
        s_oh = oh;
        s_dsc.header.w = ow;
        s_dsc.header.h = oh;
        s_dsc.header.stride = ow * 2;
        s_dsc.data_size = px * 2;
        s_dsc.data = reinterpret_cast<const uint8_t *>(s_frame);
        for (size_t i = 0; i < px; i++) {
            s_frame[i] = s_background;
        }
        lv_image_set_src(s_img, &s_dsc);
        /* LV_SCALE_NONE (a plain copy) unless smaller than SIZE */
        const int oside = ow > oh ? ow : oh;
        lv_image_set_scale(s_img, oside < SIZE ? LV_SCALE_NONE * SIZE / oside : LV_SCALE_NONE);
    }
    return true;
}

} // namespace

extern "C" lv_obj_t *assistant_emote_create(lv_obj_t *parent, uint32_t background)
{
    s_background = rgb565(background);
    eaf_dec_set_transparent_color(s_background);     // the pack draws on a cleared background
    s_dsc = {};
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_w = s_h = s_ow = s_oh = 0;
    s_img = lv_image_create(parent);
    lv_obj_set_size(s_img, SIZE, SIZE);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
    s_emotion[0] = '\0';
    s_timer = lv_timer_create(on_timer, 50, nullptr);
    assistant_emote_set("neutral");
    return s_img;
}

namespace {

/* The card's animation of MAP[m]: <emotion>.eaf, else pack_<name>.eaf; false when neither is there or usable */
bool open_card(int m)
{
    char pack[40];
    snprintf(pack, sizeof(pack), "pack_%s", MAP[m].pack);
    size_t size = 0;
    s_file = read_card(MAP[m].emotion, &size);
    if (!s_file) {
        s_file = read_card(pack, &size);
    }
    if (!s_file) {
        return false;
    }
    if (!open_animation(s_file, size)) {
        ESP_LOGW(TAG, "the card's animation of \"%s\" cannot be used", MAP[m].emotion);
        close_animation();
        return false;
    }
    return true;
}

} // namespace

extern "C" void assistant_emote_set(const char *emotion)
{
    if (!s_img) {
        return;
    }
    int m = 0;      /* unknown names play "neutral", the first entry */
    for (int i = 0; i < static_cast<int>(sizeof(MAP) / sizeof(MAP[0])); i++) {
        if (emotion && strcmp(emotion, MAP[i].emotion) == 0) {
            m = i;
            break;
        }
    }
    const bool card = card_mounted();
    /* the same face again: kept, unless it is the built-in one and the card is there now (the app was opened
     * while the card was not ready, or lent to a PC) */
    if (strcmp(s_emotion, MAP[m].emotion) == 0 && s_dec && (s_from_card || !card)) {
        return;
    }
    strlcpy(s_emotion, MAP[m].emotion, sizeof(s_emotion));
    close_animation();
    s_index = 0;

    /* A card that brings faces but not this one plays its own neutral: the built-in set is another character,
     * mixing the two looks broken (a set made for the 21 emotions of the protocol has no "idle") */
    int fps = MAP[m].fps;
    s_from_card = card && open_card(m);
    if (card && !s_from_card && m != 0 && open_card(0)) {
        ESP_LOGI(TAG, "\"%s\" is not on the card: its \"neutral\" plays", MAP[m].emotion);
        s_from_card = true;
        fps = MAP[0].fps;
    }
    if (!s_from_card && !open_animation(MAP[m].start, MAP[m].end - MAP[m].start)) {
        ESP_LOGE(TAG, "cannot open the animation of \"%s\"", MAP[m].emotion);
    }
    lv_timer_set_period(s_timer, 1000 / fps);
    show_next();
}

extern "C" void assistant_emote_destroy(void)
{
    /* the timer was created while the app was starting: the Brookesia core deletes it with the app */
    s_timer = nullptr;
    close_animation();
    s_img = nullptr;
    s_emotion[0] = '\0';
    s_from_card = false;
    heap_caps_free(s_frame);
    s_frame = nullptr;
    s_frame_px = 0;
    s_w = s_h = s_ow = s_oh = 0;
}
