/*
 * Music app: plays the audio files of the microSD card (MP3, WAV, FLAC, AAC, M4A, OGG) on the
 * speaker. Files are taken from the card and up to three folder levels below its top.
 *
 * - music::Service (music/, from esp_chatbot_v1) lists the files and reads the ID3 tag and the
 *   duration of the MP3 files in a background thread: the track that is playing first, then the
 *   rows on the screen, and nothing while the list is being scrolled.
 * - The cover is the JPEG picture of the ID3v2 tag, when there is one: decoded by the hardware
 *   JPEG decoder and scaled by the PPA before the track starts. Otherwise a colored placeholder.
 * - A player task reads the file, decodes it with esp_audio_codec and writes mono 16-bit PCM to
 *   the speaker codec. It keeps playing while the app is in the background and while the screen
 *   is off; closing the app stops it. The volume is the phone volume, followed while playing.
 */
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <strings.h>
#include <vector>
#include "phone_paths.h"
#include "decoder/esp_audio_dec_default.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "MUSIC"
#include "esp_lib_utils.h"
#include "music_service.hpp"
#include "phone_ui.h"
#include "phone_audio.h"
#include "phone_fonts.h"
#include "phone_locale.h"
#include "phone_storage.h"
#include "phone_lock.h"
#include "phone_quick.h"
#include "phone_spectrum.hpp"
#include "phone_app_music.hpp"

#define APP_NAME "Nhạc"

extern "C" const lv_image_dsc_t phone_music_icon;
extern "C" const lv_image_dsc_t phone_music_star;
extern "C" const lv_image_dsc_t phone_music_star_outline;

using namespace esp_brookesia::systems;

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "phone_music";

constexpr const char *CARD = PHONE_SD_ROOT;
constexpr size_t IN_SIZE = 4096;
constexpr size_t PCM_SIZE = 16 * 1024;
constexpr int CARD_H = 392;         // now-playing card
constexpr int ART = 150;            // artwork placeholder
constexpr int ROW_H = 60;
constexpr int ROW_GAP = 6;

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E252C;
constexpr uint32_t COLOR_ROW_CUR = 0x3A2030;
constexpr uint32_t COLOR_SUB = 0x9AA5B1;
constexpr uint32_t COLOR_ACCENT = 0xFF5F78;

/* Artwork placeholder: gradient and two discs, one palette per track (from esp_chatbot_v1) */
struct Palette {
    uint32_t start, end, primary, secondary;
};
const Palette PALETTES[] = {
    {0xFF6B35, 0xFFB347, 0xFFD166, 0xEF476F}, {0x253C78, 0x6A4C93, 0x4CC9F0, 0xF72585},
    {0x111827, 0x30475E, 0x8B5CF6, 0x22D3EE}, {0x075985, 0x0E7490, 0x67E8F9, 0x38BDF8},
    {0x92400E, 0xF59E0B, 0xFDE68A, 0xFB7185}, {0x312E81, 0x7C3AED, 0xC4B5FD, 0xF472B6},
    {0x14532D, 0x15803D, 0x86EFAC, 0xFDE047}, {0x7F1D1D, 0xBE123C, 0xFDA4AF, 0xFDBA74},
};
constexpr int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);
const Palette PALETTE_IDLE = {0x242936, 0x30475E, 0x3A4660, 0x2C3548};

enum Repeat { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE, REPEAT_COUNT };

const char *s_hold_name;    // holder of the card while the app runs (phone_storage_hold)
std::unique_ptr<music::Service> s_lib;  // created in run(), destroyed in close(); the player only runs in between

/* ---- cover ---- */

bool has_ext(const char *path, const char *ext);

constexpr int COVER_ALIGN = 128;            // L2 cache line, required for PPA output buffers
constexpr size_t COVER_FILE_MAX = 3 * 1024 * 1024;
constexpr int COVER_SIDE_MAX = 2048;
enum { COVER_NONE = -1, COVER_ABSENT = 2 }; // s_cover_ready: nothing new, buffer 0 or 1, no cover

uint8_t *s_cover_buf[2];                    // ART x ART RGB565
size_t s_cover_buf_size;
lv_image_dsc_t s_cover_dsc[2];
int s_cover_side[2];
int s_cover_front = -1;                     // buffer on the screen (LVGL task)
std::atomic<int> s_cover_ready{COVER_NONE}; // set by the player task, taken by the LVGL timer

uint32_t be32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint32_t syncsafe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0] & 0x7F) << 21 | static_cast<uint32_t>(p[1] & 0x7F) << 14 |
           static_cast<uint32_t>(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

/*
 * Find the attached picture of an ID3v2 tag (APIC, or PIC in v2.2) and return the offset and
 * the size of its JPEG data. Other picture formats (PNG) are not handled.
 */
bool find_cover(FILE *file, long &offset, size_t &size)
{
    uint8_t h[10];
    if (fseek(file, 0, SEEK_SET) != 0 || fread(h, 1, 10, file) != 10 || memcmp(h, "ID3", 3) != 0) {
        return false;
    }
    const int ver = h[3];
    const long tag_end = 10 + static_cast<long>(syncsafe32(h + 6));
    long pos = 10;
    if (h[5] & 0x40) {                      /* extended header */
        uint8_t e[4];
        if (fread(e, 1, 4, file) != 4) {
            return false;
        }
        pos += ver == 4 ? syncsafe32(e) : be32(e) + 4;
    }
    const int head = ver == 2 ? 6 : 10;
    while (pos + head < tag_end) {
        uint8_t f[10];
        if (fseek(file, pos, SEEK_SET) != 0 || fread(f, 1, head, file) != static_cast<size_t>(head) || f[0] == 0) {
            return false;                   /* padding or the end of the tag */
        }
        const uint32_t len = ver == 2 ? (static_cast<uint32_t>(f[3]) << 16 | static_cast<uint32_t>(f[4]) << 8 | f[5])
                             : (ver == 4 ? syncsafe32(f + 4) : be32(f + 4));
        const bool picture = ver == 2 ? memcmp(f, "PIC", 3) == 0 : memcmp(f, "APIC", 4) == 0;
        if (picture && len > 16) {
            /* text encoding, MIME type, picture type and description come first: look for the
             * JPEG start marker instead of parsing them */
            uint8_t probe[512];
            const size_t n = fread(probe, 1, len < sizeof(probe) ? len : sizeof(probe), file);
            for (size_t i = 0; i + 2 < n; i++) {
                if (probe[i] == 0xFF && probe[i + 1] == 0xD8 && probe[i + 2] == 0xFF) {
                    offset = pos + head + static_cast<long>(i);
                    size = len - i;
                    return true;
                }
            }
            return false;
        }
        pos += head + static_cast<long>(len);
    }
    return false;
}

/* Decode the cover of a file into buffer `slot`; false when there is none or it cannot be decoded */
bool load_cover(const std::string &path, int slot)
{
    bool ok = false;
    long offset = 0;
    size_t size = 0;
    size_t in_size = 0, out_size = 0;
    uint8_t *in = nullptr;
    uint8_t *rgb = nullptr;
    jpeg_decoder_handle_t dec = nullptr;
    ppa_client_handle_t ppa = nullptr;
    const jpeg_decode_memory_alloc_cfg_t in_cfg = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    const jpeg_decode_memory_alloc_cfg_t out_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    const jpeg_decode_engine_cfg_t eng_cfg = {.intr_priority = 0, .timeout_ms = 2000};
    const ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM};
    jpeg_decode_picture_info_t info = {};

    FILE *file = has_ext(path.c_str(), ".mp3") ? fopen(path.c_str(), "rb") : nullptr;
    if (!file || !s_cover_buf[slot] || !find_cover(file, offset, size) || size > COVER_FILE_MAX) {
        goto done;
    }
    in = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(size, &in_cfg, &in_size));
    if (!in || fseek(file, offset, SEEK_SET) != 0 || fread(in, 1, size, file) != size ||
            jpeg_decoder_get_info(in, size, &info) != ESP_OK || info.width == 0 || info.height == 0 ||
            info.width > COVER_SIDE_MAX || info.height > COVER_SIDE_MAX) {
        goto done;
    }
    {
        const int w = info.width, h = info.height;
        const int buf_w = (w + 15) & ~15, buf_h = (h + 15) & ~15;
        rgb = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(static_cast<size_t>(buf_w) * buf_h * 2, &out_cfg, &out_size));
        const jpeg_decode_cfg_t cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t len = 0;
        if (!rgb || jpeg_new_decoder_engine(&eng_cfg, &dec) != ESP_OK ||
                jpeg_decoder_process(dec, &cfg, in, size, rgb, out_size, &len) != ESP_OK ||
                ppa_register_client(&ppa_cfg, &ppa) != ESP_OK) {
            ESP_LOGW(TAG, "cover of %s cannot be decoded (%d x %d)", path.c_str(), w, h);
            goto done;
        }
        /* centered square, scaled in 1/16 steps to at most ART pixels */
        const int side = w < h ? w : h;
        int steps = (ART * 16 + side - 1) / side;
        steps = steps < 1 ? 1 : (steps > 64 ? 64 : steps);
        const int block = (side < ART * 16 / steps ? side : ART * 16 / steps) & ~1;
        const int out = block * steps / 16;
        ppa_srm_oper_config_t op = {};
        op.in.buffer = rgb;
        op.in.pic_w = buf_w;
        op.in.pic_h = buf_h;
        op.in.block_w = block;
        op.in.block_h = block;
        op.in.block_offset_x = (w - block) / 2;
        op.in.block_offset_y = (h - block) / 2;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = s_cover_buf[slot];
        op.out.buffer_size = s_cover_buf_size;
        op.out.pic_w = out;
        op.out.pic_h = out;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x = steps / 16.0f;
        op.scale_y = steps / 16.0f;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        ok = out > 0 && out <= ART && ppa_do_scale_rotate_mirror(ppa, &op) == ESP_OK;
        s_cover_side[slot] = out;
    }
done:
    if (ppa) {
        ppa_unregister_client(ppa);
    }
    if (dec) {
        jpeg_del_decoder_engine(dec);
    }
    free(rgb);
    free(in);
    if (file) {
        fclose(file);
    }
    return ok;
}

/* ---- player ---- */

std::atomic<TaskHandle_t> s_task;
std::atomic<bool> s_quit;               // the player task ends (the app is closed)
std::atomic<int> s_req{-1};             // track to start
std::atomic<bool> s_stop;
std::atomic<bool> s_paused;
std::atomic<bool> s_playing;
std::atomic<int> s_cur{-1};
std::atomic<int> s_file_permille;       // position in the file, 0..1000
std::atomic<int> s_elapsed;             // seconds of audio played
std::atomic<bool> s_error;              // the last track could not be played
std::atomic<int> s_repeat{REPEAT_OFF};
std::atomic<int> s_seek{-1};            // position asked by the progress slider, 0..1000
std::atomic<bool> s_seekable;           // the playing file can be sought (not M4A / OGG)
std::atomic<bool> s_viz_feed;           // the spectrum is on the screen: feed it the sound

/* Tracks in the order of the list on the screen (all, or the favorites only), and the order they
 * play in: the same, or shuffled. Previous, next and the end of the list follow the play order.
 * Written by the LVGL task, read by the player task. */
std::mutex s_order_mtx;
std::vector<int> s_order;
std::vector<int> s_play;
std::atomic<bool> s_shuffle;

/* s_play from s_order: shuffled (each track once per round) with `first` at its head, or as
 * listed. Called with s_order_mtx held. */
void make_play_order_locked(int first)
{
    s_play = s_order;
    if (!s_shuffle) {
        return;
    }
    for (size_t i = s_play.size(); i > 1; i--) {
        std::swap(s_play[i - 1], s_play[esp_random() % i]);
    }
    for (size_t i = 0; i < s_play.size(); i++) {
        if (s_play[i] == first) {
            std::swap(s_play[0], s_play[i]);
            break;
        }
    }
}

void make_play_order(int first)
{
    std::lock_guard<std::mutex> lock(s_order_mtx);
    make_play_order_locked(first);
}

/* Track after (step 1) or before (step -1) idx in the play order; -1 at an end unless wrap.
 * Shuffled and wrapping at the end: a new round in a new order. */
int order_step(int idx, int step, bool wrap)
{
    std::lock_guard<std::mutex> lock(s_order_mtx);
    const int n = static_cast<int>(s_play.size());
    if (n == 0) {
        return -1;
    }
    int pos = -1;
    for (int i = 0; i < n; i++) {
        if (s_play[i] == idx) {
            pos = i;
            break;
        }
    }
    if (pos < 0) {
        if (s_shuffle) {
            return step > 0 || wrap ? s_play[0] : -1;
        }
        /* not in the list (unmarked while only the favorites are shown): the next one after it */
        for (int i = 0; i < n; i++) {
            if (s_play[i] > idx) {
                return step > 0 ? s_play[i] : (i > 0 ? s_play[i - 1] : (wrap ? s_play[n - 1] : -1));
            }
        }
        return step > 0 ? (wrap ? s_play[0] : -1) : s_play[n - 1];
    }
    const int next = pos + step;
    if (next >= 0 && next < n) {
        return s_play[next];
    }
    if (!wrap) {
        return -1;
    }
    if (s_shuffle && step > 0 && n > 1) {
        make_play_order_locked(-1);
        if (s_play[0] == idx) {             /* not the same track twice in a row */
            std::swap(s_play[0], s_play[n - 1]);
        }
        return s_play[0];
    }
    return s_play[(next + n) % n];
}

bool has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

esp_audio_simple_dec_type_t type_of(const char *path)
{
    static const struct {
        const char *ext;
        esp_audio_simple_dec_type_t type;
    } map[] = {
        {".mp3", ESP_AUDIO_SIMPLE_DEC_TYPE_MP3}, {".wav", ESP_AUDIO_SIMPLE_DEC_TYPE_WAV},
        {".flac", ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC}, {".aac", ESP_AUDIO_SIMPLE_DEC_TYPE_AAC},
        {".m4a", ESP_AUDIO_SIMPLE_DEC_TYPE_M4A}, {".ogg", ESP_AUDIO_SIMPLE_DEC_TYPE_OGG},
    };
    for (const auto &m : map) {
        if (has_ext(path, m.ext)) {
            return m.type;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

bool interrupted()
{
    return s_stop || s_req >= 0;
}

/* Where the audio data starts: after the ID3v2 tag of an MP3, at the "data" chunk of a WAV
 * (block_align is set for it), 0 otherwise */
long audio_start(FILE *file, esp_audio_simple_dec_type_t type, int &block_align)
{
    block_align = 1;
    uint8_t h[512];
    if (fseek(file, 0, SEEK_SET) != 0) {
        return 0;
    }
    const size_t n = fread(h, 1, sizeof(h), file);
    long start = 0;
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 && n >= 10 && memcmp(h, "ID3", 3) == 0) {
        start = 10 + static_cast<long>(syncsafe32(h + 6)) + ((h[5] & 0x10) ? 10 : 0);
    } else if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV && n >= 12 && memcmp(h, "RIFF", 4) == 0) {
        for (size_t i = 12; i + 8 <= n;) {
            const uint32_t len = h[i + 4] | h[i + 5] << 8 | h[i + 6] << 16 | static_cast<uint32_t>(h[i + 7]) << 24;
            if (memcmp(h + i, "fmt ", 4) == 0 && i + 22 <= n) {
                block_align = h[i + 20] | h[i + 21] << 8;
            } else if (memcmp(h + i, "data", 4) == 0) {
                start = static_cast<long>(i + 8);
                break;
            }
            i += 8 + ((len + 1) & ~1u);
        }
        block_align = block_align < 1 ? 1 : block_align;
    }
    fseek(file, 0, SEEK_SET);
    return start;
}

void register_decoders()
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
}

/* Play one file to its end; returns false when it could not be played */
bool play_file(const std::string &path)
{
    bool ok = false;
    bool opened = false;
    int channels = 1;
    uint32_t rate = 0;
    uint64_t samples = 0;
    int volume = -1;
    size_t pcm_size = PCM_SIZE;
    esp_audio_simple_dec_handle_t dec = nullptr;
    auto *in = static_cast<uint8_t *>(malloc(IN_SIZE));
    auto *pcm = static_cast<uint8_t *>(malloc(pcm_size));
    FILE *file = fopen(path.c_str(), "rb");
    /* the newest player wins: the radio (or a video) taking the speaker stops the music */
    auto spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take_yielding([]() {
        s_req = -1;
        s_stop = true;
    }));
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type_of(path.c_str());
    long size = 0;
    long data_start = 0;
    int block_align = 1;
    /* MP3 and ADTS AAC are frame streams: after a jump the decoder is reset and finds the next
     * frame. WAV and FLAC keep their decoder (a reset would wait for the file header again). */
    const bool frame_stream = cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 || cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    long resync = 0;                    // input bytes after a jump in which decode errors are skipped
    if (file) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        data_start = audio_start(file, cfg.dec_type, block_align);
    }
    s_seek = -1;
    s_seekable = file && size > data_start && (frame_stream || cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV ||
                 cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC);

    if (in && pcm && file && size > 0 && spk && esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        /* The GMF player of the Radio app unregisters the default decoders when it is destroyed (its
         * own reference count): register them again */
        register_decoders();
        if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
            dec = nullptr;
        }
    }
    if (!in || !pcm || !file || size <= 0 || !spk || !dec) {
        ESP_LOGE(TAG, "cannot start %s", path.c_str());
        goto done;
    }
    ok = true;
    while (!interrupted()) {
        const int seek = s_seek.exchange(-1);
        if (seek >= 0 && s_seekable) {
            /* jump by position in the file: exact for CBR, close for VBR */
            const long pos = ftell(file);
            long target = 0;
            if (seek > 0) {
                target = data_start + static_cast<long>(static_cast<int64_t>(size - data_start) * seek / 1000);
                target = data_start + (target - data_start) / block_align * block_align;
            }
            music::TrackView view;
            const uint32_t duration_ms = s_lib && s_lib->snapshot(s_cur, view) ? view.duration_ms : 0;
            if (seek == 0) {
                samples = 0;
            } else if (duration_ms && rate) {
                samples = static_cast<uint64_t>(duration_ms) * seek / 1000 * rate / 1000;
            } else if (pos > data_start) {
                samples = samples * static_cast<uint64_t>(target - data_start) / static_cast<uint64_t>(pos - data_start);
            }
            fseek(file, target, SEEK_SET);
            if (seek == 0 || frame_stream) {
                esp_audio_simple_dec_reset(dec);     /* from the start: every format takes it */
            }
            resync = seek == 0 ? 0 : 64 * 1024;
            s_elapsed = rate ? static_cast<int>(samples / rate) : 0;
            s_file_permille = seek;
        }
        const size_t n = fread(in, 1, IN_SIZE, file);
        if (n == 0) {
            break;
        }
        resync -= resync > 0 ? static_cast<long>(n) : 0;
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = in;
        raw.len = n;
        raw.eos = feof(file) != 0;
        while (raw.len > 0 && !interrupted() && s_seek < 0) {
            esp_audio_simple_dec_out_t out = {};
            out.buffer = pcm;
            out.len = pcm_size;
            const esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &out);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                auto *bigger = static_cast<uint8_t *>(realloc(pcm, out.needed_size));
                if (!bigger) {
                    ok = false;
                    goto done;
                }
                pcm = bigger;
                pcm_size = out.needed_size;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK && resync > 0) {
                break;          /* landed inside a frame after a jump: drop this input and read on */
            }
            if (err != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "decode error %d in %s", err, path.c_str());
                ok = opened;        /* a broken tail still counts as played */
                goto done;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (out.decoded_size == 0) {
                continue;
            }
            if (!opened) {
                esp_audio_simple_dec_info_t info = {};
                esp_audio_simple_dec_get_info(dec, &info);
                if (info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2 || info.sample_rate == 0) {
                    ESP_LOGE(TAG, "unsupported format: %u Hz, %u bit, %u ch", static_cast<unsigned>(info.sample_rate),
                             info.bits_per_sample, info.channel);
                    ok = false;
                    goto done;
                }
                channels = info.channel;
                rate = info.sample_rate;
                esp_codec_dev_sample_info_t fs = {};
                fs.sample_rate = rate;
                fs.channel = 1;
                fs.bits_per_sample = 16;
                if (esp_codec_dev_open(spk, &fs) != ESP_CODEC_DEV_OK) {
                    ok = false;
                    goto done;
                }
                opened = true;
                ESP_LOGI(TAG, "%s: %u Hz, %d ch", path.c_str(), static_cast<unsigned>(rate), channels);
            }
            /* the speaker is mono: mix stereo down in place */
            auto *s = reinterpret_cast<int16_t *>(pcm);
            size_t frames = out.decoded_size / (2 * channels);
            if (channels == 2) {
                for (size_t i = 0; i < frames; i++) {
                    s[i] = static_cast<int16_t>((s[2 * i] + s[2 * i + 1]) / 2);
                }
            }
            const int v = phone_audio_get_volume();
            if (v != volume) {
                volume = v;
                esp_codec_dev_set_out_vol(spk, v);
            }
            while (s_paused && !interrupted() && s_seek < 0) {
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            if (s_seek >= 0) {
                break;          /* audio from before the jump is not played */
            }
            esp_codec_dev_write(spk, s, frames * 2);
            if (s_viz_feed && v > 0) {
                spectrum::feed(s, static_cast<int>(frames), static_cast<int>(rate));   /* muted: the bars fall */
            }
            samples += frames;
            s_elapsed = static_cast<int>(samples / rate);
            s_file_permille = static_cast<int>(ftell(file) * 1000LL / size);
        }
    }
done:
    if (opened) {
        esp_codec_dev_close(spk);
    }
    if (spk) {
        phone_audio_speaker_give();
    }
    if (dec) {
        esp_audio_simple_dec_close(dec);
    }
    if (file) {
        fclose(file);
    }
    free(in);
    free(pcm);
    return ok;
}

void player_task(void *)
{
    static bool s_registered;
    if (!s_registered) {
        register_decoders();
        s_registered = true;
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (s_quit) {
            s_task = nullptr;
            vTaskDelete(nullptr);
        }
        int idx = s_req.exchange(-1);
        std::string path;
        while (idx >= 0 && !s_stop && s_lib && s_lib->path(idx, path)) {
            s_cur = idx;
            s_file_permille = 0;
            s_elapsed = 0;
            s_paused = false;
            s_error = false;
            s_playing = true;
            {
                /* into the buffer that is not on the screen */
                const int slot = s_cover_front == 0 ? 1 : 0;
                s_cover_ready = load_cover(path, slot) ? slot : static_cast<int>(COVER_ABSENT);
            }
            const bool ok = play_file(path);
            s_error = !ok;
            const int req = s_req.exchange(-1);
            const int count = s_lib ? static_cast<int>(s_lib->count()) : 0;
            if (req >= 0) {
                idx = req;                          /* another track was chosen */
            } else if (s_stop || !ok) {
                idx = -1;
            } else if (s_repeat == REPEAT_ONE) {
                /* same track again */
            } else {
                /* next in the list on the screen; at its end, the first again or stop */
                idx = count > 0 ? order_step(idx, 1, s_repeat == REPEAT_ALL) : -1;
            }
        }
        s_playing = false;
        s_stop = false;
    }
}

void play_track(int idx)
{
    if (!s_lib || idx < 0 || idx >= static_cast<int>(s_lib->count())) {
        return;
    }
    s_quit = false;
    if (!s_task) {
        TaskHandle_t task = nullptr;
        if (xTaskCreate(player_task, "phone_music", 8192, nullptr, 5, &task) != pdPASS) {
            return;
        }
        s_task = task;
    }
    s_stop = false;
    s_req = idx;
    xTaskNotifyGive(s_task);
}

void stop_playback()
{
    if (!s_task) {
        return;
    }
    s_req = -1;
    if (s_playing) {
        s_stop = true;
    }
    for (int i = 0; i < 100 && s_playing; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Closing the app ends the player task: its 8 KB stack is internal RAM */
void end_player()
{
    stop_playback();
    if (!s_task || s_playing) {
        return;                             /* still playing after 2 s: left running */
    }
    s_quit = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 50 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---- library ---- */

/* Stops the library service away from the LVGL task. pthread_join() of ESP-IDF waits with
 * xTaskNotifyWait(), which any task notification ends, and the LVGL task gets them all the time
 * from the adapter: a join there returned early and deleted the scanning thread while it was still
 * running (holding the pthread list lock), so the next thread creation hung the UI (Music closed
 * from Recents, then opened again) */
void retire_library()
{
    music::Service *lib = s_lib.release();
    if (!lib) {
        return;
    }
    auto stop = [](void *arg) {
        delete static_cast<music::Service *>(arg);
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(stop, "music_stop", 4096, lib, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "cannot stop the library service");      // leaked rather than joined here
    }
}

void start_library()
{
    music::Service::Config config;
    config.root = CARD;
    /* tag and duration only for MP3; the other formats keep their file name */
    config.parse = [](const char *path, music::TrackMeta & meta, const music::ParseOptions & options) {
        if (!has_ext(path, ".mp3")) {
            memset(&meta, 0, sizeof(meta));
            return music::ParseResult::IoError;
        }
        return music::parse_mp3_file(path, meta, options);
    };
    config.thread_setup = []() {
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.thread_name = "music_meta";
        cfg.stack_size = 6144;
        cfg.prio = 3;
        cfg.inherit_cfg = false;
        esp_pthread_set_cfg(&cfg);
    };
    s_lib = std::make_unique<music::Service>(std::move(config));
    if (phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED) {
        s_lib->start();
    }
}

/* ---- UI ---- */

struct Row {
    int idx;                // track of the library
    lv_obj_t *obj;
    lv_obj_t *number;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *duration;
    lv_obj_t *star;
    music::MetaState shown;
};

/* ---- favorites: one path per line, relative to the card, in a text file of the card ---- */

constexpr const char *FAV_FILE = PHONE_SD_MUSIC_FAVORITES;
std::unordered_set<std::string> s_favs;
bool s_only_fav;                // the list shows the favorites only

std::string rel_path(const std::string &path)
{
    const size_t root = strlen(CARD);
    return path.compare(0, root, CARD) == 0 && path.size() > root + 1 ? path.substr(root + 1) : path;
}

void load_favorites()
{
    s_favs.clear();
    FILE *f = fopen(FAV_FILE, "r");
    if (!f) {
        f = fopen(PHONE_SD_OLD_MUSIC_FAVORITES, "r");       /* v0.1.0; the next save writes the new place */
    }
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
            line[--n] = '\0';
        }
        const char *p = line[0] == '/' ? line + 1 : line;
        if (*p && *p != '#') {
            s_favs.insert(p);
        }
    }
    fclose(f);
}

bool save_favorites()
{
    std::vector<std::string> lines(s_favs.begin(), s_favs.end());
    std::sort(lines.begin(), lines.end());
    phone_sd_make_dirs(FAV_FILE);
    FILE *f = fopen(FAV_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", FAV_FILE);
        return false;
    }
    bool ok = true;
    for (const auto &l : lines) {
        ok = ok && fprintf(f, "%s\n", l.c_str()) > 0;
    }
    ok = fclose(f) == 0 && ok;
    return ok;
}

bool is_fav(int idx)
{
    std::string path;
    return s_lib && idx >= 0 && s_lib->path(idx, path) && s_favs.count(rel_path(path)) != 0;
}

lv_obj_t *s_root;
lv_obj_t *s_title;
lv_obj_t *s_artist;
lv_obj_t *s_time;
lv_obj_t *s_duration;
lv_obj_t *s_art;
lv_obj_t *s_art_primary;
lv_obj_t *s_art_secondary;
lv_obj_t *s_cover_img;
lv_obj_t *s_art_content;        // placeholder discs, note and cover
lv_obj_t *s_viz;                // spectrum over the artwork, shown or not by a tap on it
spectrum::View s_viz_view;
bool s_show_viz;
lv_obj_t *s_mute_badge;         // over the artwork while the volume is 0
lv_obj_t *s_mute_icon;
uint32_t s_mute_since;
lv_obj_t *s_bar;                // progress slider, 0..1000
lv_obj_t *s_play_label;
lv_obj_t *s_repeat_label;
lv_obj_t *s_repeat_tag;         // "ALL" or "1" right of the repeat icon
lv_obj_t *s_shuffle_label;
lv_obj_t *s_filter_label;
bool s_dragging;                // the progress slider is held
std::vector<int> s_row_of;      // row of each track of the library, -1 when not in the list
lv_obj_t *s_list;
lv_obj_t *s_empty;
std::vector<Row> s_rows;
bool s_rows_built;
uint32_t s_seen_version;
int s_shown_cur = -2;
int s_shown_state = -1;
uint32_t s_cur_duration_ms;

std::string path_of(int idx)
{
    std::string path;
    if (s_lib) {
        s_lib->path(idx, path);
    }
    return path;
}

/* m:ss, or h:mm:ss from one hour on (mixes of two hours and more) */
void format_time(char *buf, size_t len, uint32_t seconds)
{
    if (seconds >= 3600) {
        snprintf(buf, len, "%u:%02u:%02u", static_cast<unsigned>(seconds / 3600), static_cast<unsigned>(seconds / 60 % 60),
                 static_cast<unsigned>(seconds % 60));
    } else {
        snprintf(buf, len, "%u:%02u", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
    }
}

void on_row(lv_event_t *e)
{
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (s_shuffle) {
        make_play_order(idx);       /* a new round that starts with the chosen track */
    }
    play_track(idx);
}

/* Star of a favorite: a fixed column just left of the duration column, so the durations stay
 * at the right edge and the stars line up */
constexpr int DURATION_W = 66;     // fits h:mm:ss
constexpr int STAR_HIT_W = 36 + DURATION_W + 14;  // star column + durations + right margin
void place_star(Row &r, bool fav)
{
    lv_image_set_src(r.star, fav ? &phone_music_star : &phone_music_star_outline);
    lv_obj_set_style_image_recolor(r.star, lv_color_hex(fav ? COLOR_ACCENT : 0x5A6672), 0);
}

/* Mark or unmark a track; the file on the card is written at once. In the favorites list an
 * unmarked track stays (outlined) until the list is built again: no rows jumping under the
 * finger, and a wrong tap can be undone. */
void toggle_fav(int idx)
{
    std::string path;
    if (!s_lib || idx < 0 || !s_lib->path(idx, path)) {
        return;
    }
    const std::string rel = rel_path(path);
    if (!s_favs.erase(rel)) {
        s_favs.insert(rel);
    }
    save_favorites();
    if (idx < static_cast<int>(s_row_of.size()) && s_row_of[idx] >= 0) {
        place_star(s_rows[s_row_of[idx]], s_favs.count(rel) != 0);
    }
}

void on_row_star(lv_event_t *e)
{
    toggle_fav(static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void refresh_rows();

void build_rows()
{
    const int count = static_cast<int>(s_lib->count());
    const int width = lv_obj_get_width(s_list) - 24;
    std::vector<int> order;
    for (int i = 0; i < count; i++) {
        if (!s_only_fav || is_fav(i)) {
            order.push_back(i);
        }
    }
    {
        std::lock_guard<std::mutex> lock(s_order_mtx);
        s_order = order;
        make_play_order_locked(s_cur);
    }
    lv_obj_clean(s_list);
    s_rows.clear();
    s_row_of.assign(count, -1);
    for (size_t pos = 0; pos < order.size(); pos++) {
        const int i = order[pos];
        Row r = {};
        r.idx = i;
        r.shown = music::MetaState::Pending;
        r.obj = lv_button_create(s_list);
        lv_obj_remove_style_all(r.obj);
        lv_obj_set_size(r.obj, LV_PCT(100), ROW_H);
        lv_obj_set_style_radius(r.obj, 12, 0);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r.obj, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(r.obj, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_add_event_cb(r.obj, on_row, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        r.number = lv_label_create(r.obj);
        lv_obj_set_style_text_color(r.number, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(r.number, phone_font_by_size(16), 0);
        lv_label_set_text_fmt(r.number, "%02d", static_cast<int>(pos) + 1);
        lv_obj_align(r.number, LV_ALIGN_LEFT_MID, 14, 0);

        music::TrackView view;
        s_lib->snapshot(i, view);
        r.title = lv_label_create(r.obj);
        lv_label_set_long_mode(r.title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.title, width - 54 - STAR_HIT_W - 6);
        lv_obj_set_height(r.title, lv_font_get_line_height(phone_font_by_size(20)));
        lv_label_set_text(r.title, view.title);
        lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 54, 0);

        r.artist = lv_label_create(r.obj);
        lv_obj_set_style_text_color(r.artist, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(r.artist, phone_font_by_size(14), 0);
        lv_label_set_long_mode(r.artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(r.artist, width - 54 - STAR_HIT_W - 6);
        lv_obj_set_height(r.artist, lv_font_get_line_height(phone_font_by_size(14)));
        lv_label_set_text(r.artist, "");
        lv_obj_align(r.artist, LV_ALIGN_LEFT_MID, 54, 14);
        lv_obj_add_flag(r.artist, LV_OBJ_FLAG_HIDDEN);

        r.duration = lv_label_create(r.obj);
        lv_obj_set_style_text_color(r.duration, lv_color_hex(COLOR_SUB), 0);
        lv_obj_set_style_text_font(r.duration, phone_font_by_size(16), 0);
        lv_label_set_text(r.duration, "");
        lv_obj_set_width(r.duration, DURATION_W);
        lv_obj_set_style_text_align(r.duration, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(r.duration, LV_ALIGN_RIGHT_MID, -14, 0);

        /* star in its column left of the durations: filled for a favorite, outlined otherwise;
         * a tap on it marks or unmarks the track (not played) */
        /* the whole right end of the row, from the star to the edge and the row's full height, is
         * the target of the star (on top of the duration, which it covers) */
        lv_obj_t *hit = lv_obj_create(r.obj);
        lv_obj_remove_style_all(hit);
        lv_obj_set_size(hit, STAR_HIT_W, LV_PCT(100));
        lv_obj_align(hit, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_remove_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(hit, 12, 0);
        lv_obj_set_style_bg_color(hit, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(hit, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_add_event_cb(hit, on_row_star, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        r.star = lv_image_create(hit);
        lv_image_set_scale(r.star, 160);       /* 32 px -> 20 px */
        lv_obj_set_style_image_recolor_opa(r.star, LV_OPA_COVER, 0);
        lv_obj_remove_flag(r.star, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(r.star, LV_ALIGN_LEFT_MID, 2, 0);   /* 32 px box, 20 px star in its middle */
        place_star(r, s_favs.count(rel_path(path_of(i))) != 0);
        s_row_of[i] = static_cast<int>(s_rows.size());
        s_rows.push_back(r);
    }
    if (count == 0) {
        lv_label_set_text(s_empty, T("Chưa có nhạc.\nChép tệp MP3, WAV, FLAC, AAC\nvào thư mục Music trên thẻ nhớ.",
                                     "No music yet.\nCopy MP3, WAV, FLAC or AAC files\nto the Music folder of the card."));
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else if (order.empty()) {
        lv_label_set_text(s_empty, T("Chưa có bài yêu thích.\nBấm nút ngôi sao khi đang phát\nđể thêm bài vào đây.",
                                     "No favorites yet.\nTap the star while a track plays\nto add it here."));
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    s_rows_built = true;
    s_shown_cur = -2;
    /* new rows start empty: fill in the tags already read (the library version does not change
     * for them, so the timer would never do it: no durations after switching the filter) */
    s_seen_version = s_lib->version();
    refresh_rows();
}

/* Fill in the rows whose tag has been read since the last time */
void refresh_rows()
{
    for (Row &r : s_rows) {
        if (r.shown != music::MetaState::Pending) {
            continue;
        }
        music::TrackView view;
        if (!s_lib->snapshot(r.idx, view) || view.state == music::MetaState::Pending) {
            continue;
        }
        r.shown = view.state;
        lv_label_set_text(r.title, view.title);
        if (view.artist[0]) {
            lv_label_set_text(r.artist, view.artist);
            lv_obj_remove_flag(r.artist, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 54, -10);
        }
        if (view.duration_ms) {
            char buf[16];
            format_time(buf, sizeof(buf), view.duration_ms / 1000);
            lv_label_set_text(r.duration, buf);
        }
    }
}

/* Ask for the tags of the rows on the screen first, then of the ones after them */
void update_wanted()
{
    if (s_rows.empty()) {
        return;
    }
    size_t wanted[music::Service::kMaxWanted];
    size_t n = 0;
    const size_t count = s_rows.size();
    const size_t first = static_cast<size_t>(lv_obj_get_scroll_y(s_list) / (ROW_H + ROW_GAP)) % count;
    for (size_t k = 0; k < count && n < music::Service::kMaxWanted; k++) {
        const size_t i = (first + k) % count;
        if (s_rows[i].shown == music::MetaState::Pending) {
            wanted[n++] = static_cast<size_t>(s_rows[i].idx);
        }
    }
    s_lib->set_wanted(wanted, n);
}

void set_artwork(const Palette &p)
{
    lv_obj_set_style_bg_color(s_art, lv_color_hex(p.start), 0);
    lv_obj_set_style_bg_grad_color(s_art, lv_color_hex(p.end), 0);
    lv_obj_set_style_bg_color(s_art_primary, lv_color_hex(p.primary), 0);
    lv_obj_set_style_bg_color(s_art_secondary, lv_color_hex(p.secondary), 0);
}

/* Current row: accent border, a note instead of its number (index: track of the library) */
void mark_row(int index, bool current)
{
    if (index < 0 || index >= static_cast<int>(s_row_of.size()) || s_row_of[index] < 0) {
        return;
    }
    const int pos = s_row_of[index];
    Row &r = s_rows[pos];
    lv_obj_set_style_bg_color(r.obj, lv_color_hex(current ? COLOR_ROW_CUR : COLOR_ROW), 0);
    lv_obj_set_style_border_width(r.obj, current ? 2 : 0, 0);
    lv_obj_set_style_text_color(r.title, current ? lv_color_hex(COLOR_ACCENT) : lv_color_white(), 0);
    lv_obj_set_style_text_color(r.number, lv_color_hex(current ? COLOR_ACCENT : COLOR_SUB), 0);
    if (current) {
        lv_label_set_text(r.number, LV_SYMBOL_AUDIO);
    } else {
        lv_label_set_text_fmt(r.number, "%02d", pos + 1);
    }
}

void show_current(int cur)
{
    mark_row(s_shown_cur, false);
    s_cur_duration_ms = 0;
    if (s_lib && cur >= 0 && cur < static_cast<int>(s_lib->count())) {
        mark_row(cur, true);
        s_lib->set_pinned(cur);
        set_artwork(PALETTES[cur % PALETTE_COUNT]);
    } else {
        set_artwork(PALETTE_IDLE);
    }
    s_shown_cur = cur;
}

/* Title, artist and duration of the current track; they may arrive after the track started */
void refresh_now_playing(int cur)
{
    music::TrackView view;
    if (cur < 0 || !s_lib->snapshot(cur, view)) {
        return;
    }
    if (strcmp(lv_label_get_text(s_title), view.title) != 0) {
        lv_label_set_text(s_title, view.title);
    }
    if (!s_error && strcmp(lv_label_get_text(s_artist), view.artist) != 0) {
        lv_label_set_text(s_artist, view.artist);
    }
    s_cur_duration_ms = view.duration_ms;
}

void on_timer(lv_timer_t *)
{
    if (!s_root || !s_lib) {
        return;
    }
    if (!s_rows_built && s_lib->scan_done()) {
        build_rows();
    }
    const uint32_t version = s_lib->version();
    if (s_rows_built && version != s_seen_version) {
        s_seen_version = version;
        refresh_rows();
    }
    if (s_rows_built) {
        update_wanted();
    }

    const int cover = s_cover_ready.exchange(COVER_NONE);
    if (cover == 0 || cover == 1) {
        lv_image_dsc_t &dsc = s_cover_dsc[cover];
        dsc = {};
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = s_cover_side[cover];
        dsc.header.h = s_cover_side[cover];
        dsc.header.stride = s_cover_side[cover] * 2;
        dsc.data_size = s_cover_side[cover] * s_cover_side[cover] * 2;
        dsc.data = s_cover_buf[cover];
        s_cover_front = cover;
        lv_image_cache_drop(&dsc);
        lv_image_set_src(s_cover_img, &dsc);
        lv_obj_center(s_cover_img);
        lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    } else if (cover == COVER_ABSENT) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    }

    const int cur = s_cur;
    const bool playing = s_playing;
    const int state = !playing ? 0 : (s_paused ? 1 : 2);
    if (cur != s_shown_cur && s_rows_built) {
        show_current(cur);
    }
    if (state != s_shown_state) {
        s_shown_state = state;
        lv_label_set_text(s_play_label, state == 2 ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (cur >= 0) {
        refresh_now_playing(cur);
        const uint32_t t = s_elapsed;
        char buf[16];
        if (!s_dragging) {          /* while dragging it shows the time under the finger */
            format_time(buf, sizeof(buf), t);
            lv_label_set_text(s_time, buf);
        }
        if (!playing && s_error) {
            lv_label_set_text(s_artist, T("Không phát được tệp này", "Cannot play this file"));
        }
        if (s_cur_duration_ms) {
            format_time(buf, sizeof(buf), s_cur_duration_ms / 1000);
            lv_label_set_text(s_duration, buf);
        } else {
            lv_label_set_text(s_duration, "--:--");
        }
        /* by time when the duration is known, by position in the file otherwise */
        int permille = s_file_permille;
        if (s_cur_duration_ms) {
            permille = static_cast<int>(static_cast<uint64_t>(t) * 1000000ULL / s_cur_duration_ms);
        }
        if (!s_dragging && s_seek < 0) {
            lv_slider_set_value(s_bar, permille > 1000 ? 1000 : permille, LV_ANIM_OFF);
        }
    }
    /* the slider moves the playback only while a file that can be sought plays */
    const bool can_seek = playing && s_seekable;
    if (can_seek != !lv_obj_has_state(s_bar, LV_STATE_DISABLED)) {
        if (can_seek) {
            lv_obj_remove_state(s_bar, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_bar, LV_STATE_DISABLED);
            s_dragging = false;
        }
    }
}

/* Progress slider: the time under the finger while dragging, the jump when it is let go */
void on_seek(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && s_dragging && s_cur_duration_ms) {
        char buf[16];
        const uint64_t ms = static_cast<uint64_t>(lv_slider_get_value(s_bar)) * s_cur_duration_ms / 1000;
        format_time(buf, sizeof(buf), static_cast<uint32_t>(ms / 1000));
        lv_label_set_text(s_time, buf);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_dragging) {
        s_dragging = false;
        if (s_playing && s_seekable) {
            s_seek = lv_slider_get_value(s_bar);
        }
    }
}

void show_shuffle()
{
    lv_obj_set_style_text_color(s_shuffle_label, lv_color_hex(s_shuffle ? COLOR_ACCENT : COLOR_SUB), 0);
}

/* Shuffle on: a new order of the list, from the current track on */
void on_shuffle(lv_event_t *)
{
    s_shuffle = !s_shuffle;
    phone_pref_set("mus_shuf", s_shuffle ? 1 : 0);
    make_play_order(s_cur);
    show_shuffle();
}

void show_filter()
{
    lv_label_set_text(s_filter_label, s_only_fav ? T(LV_SYMBOL_LIST "  Yêu thích", LV_SYMBOL_LIST "  Favorites")
                      : T(LV_SYMBOL_LIST "  Tất cả", LV_SYMBOL_LIST "  All"));
    lv_obj_set_style_text_color(s_filter_label, lv_color_hex(s_only_fav ? COLOR_ACCENT : COLOR_SUB), 0);
}

/* All tracks / the favorites only */
void on_filter(lv_event_t *)
{
    s_only_fav = !s_only_fav;
    phone_pref_set("mus_fav", s_only_fav ? 1 : 0);
    show_filter();
    if (s_rows_built) {
        build_rows();
    }
}

void on_play(lv_event_t *)
{
    if (!s_lib || s_lib->count() == 0) {
        return;
    }
    if (s_playing) {
        s_paused = !s_paused;
    } else if (s_cur >= 0) {
        play_track(s_cur);
    } else {
        play_track(order_step(-1, 1, false));       /* the first of the list */
    }
}

/* Previous and next follow the list on the screen (all tracks or the favorites) */
void on_prev(lv_event_t *)
{
    const int cur = s_cur;
    const int prev = cur >= 0 ? order_step(cur, -1, false) : -1;
    play_track(prev >= 0 ? prev : (cur >= 0 ? cur : order_step(-1, 1, false)));
}

void on_next(lv_event_t *)
{
    const int next = order_step(s_cur, 1, s_repeat == REPEAT_ALL);
    if (next >= 0) {
        play_track(next);
    }
}

/* The icon stays in place; "ALL" (whole list) or "1" (this track) in small letters right of it */
void show_repeat()
{
    const int mode = s_repeat;
    const lv_color_t color = lv_color_hex(mode == REPEAT_OFF ? COLOR_SUB : COLOR_ACCENT);
    lv_obj_set_style_text_color(s_repeat_label, color, 0);
    lv_obj_set_style_text_color(s_repeat_tag, color, 0);
    lv_label_set_text(s_repeat_tag, mode == REPEAT_ALL ? "ALL" : (mode == REPEAT_ONE ? "1" : ""));
}

/* off, whole list, one track */
void on_repeat(lv_event_t *)
{
    s_repeat = (s_repeat + 1) % REPEAT_COUNT;
    phone_pref_set("mus_rep", s_repeat);
    show_repeat();
}

/* Cover or spectrum in the artwork. The rounded corners of the artwork clip its children
 * (clip_corner): LVGL then draws every change through a layer, which cost about 40 % of a core with
 * the bars moving 25 times a second. The spectrum has its own rounded background and its bars stay
 * inside its padding, so the clip is off while it shows. */
void show_viz_mode()
{
    lv_obj_set_style_clip_corner(s_art, !s_show_viz, 0);
    if (s_show_viz) {
        spectrum::view_clear(s_viz_view);
        lv_obj_remove_flag(s_viz, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_art_content, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_viz, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_art_content, LV_OBJ_FLAG_HIDDEN);
        s_viz_feed = false;
    }
}

/* A tap on the artwork: the cover (or its placeholder) or the spectrum; remembered */
void on_art(lv_event_t *)
{
    s_show_viz = !s_show_viz;
    phone_pref_set("mus_viz", s_show_viz ? 1 : 0);
    show_viz_mode();
}

/* About 25 times a second: the bars while they are on the screen, and the mute badge */
void on_viz_timer(lv_timer_t *)
{
    if (!s_root) {
        return;
    }
    /* not under the quick settings panel either: it covers the artwork, and each bar change would
     * redraw the panel over it */
    const bool on_screen = lv_obj_get_screen(s_root) == lv_screen_active() && !phone_lock_is_locked() &&
                           !phone_quick_visible();
    const bool muted = phone_audio_get_volume() == 0;
    s_viz_feed = s_show_viz && on_screen && s_playing && !s_paused && !muted;
    if (s_show_viz && on_screen) {
        /* the levels are shared: another player (the radio in the background) feeds them too */
        static bool cleared;
        if (s_playing && !s_paused) {
            spectrum::view_update(s_viz_view);
            cleared = false;
        } else if (!cleared) {
            spectrum::view_clear(s_viz_view);       /* once: it restyles every bar */
            cleared = true;
        }
    }
    /* muted: a dark disc with a crossed speaker, breathing (2.4 s, from full to 15 %), over the cover
     * or the bars */
    if (muted != !lv_obj_has_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN)) {
        if (muted) {
            lv_obj_remove_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN);
            s_mute_since = lv_tick_get();
        } else {
            lv_obj_add_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (muted && on_screen) {
        const uint32_t phase = lv_tick_elaps(s_mute_since) % 2400;
        const uint32_t fade = phase <= 1200 ? phase : 2400 - phase;
        lv_obj_set_style_image_opa(s_mute_icon, static_cast<lv_opa_t>(LV_OPA_COVER - fade * 215 / 1200), 0);
    }
}

void on_scroll(lv_event_t *e)
{
    if (s_lib) {
        s_lib->set_ui_idle(lv_event_get_code(e) == LV_EVENT_SCROLL_END);
    }
}

lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, int size, uint32_t color, lv_event_cb_t cb,
                       lv_obj_t **label = nullptr)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, symbol);
    lv_obj_center(l);
    if (label) {
        *label = l;
    }
    return b;
}

void build_ui()
{
    s_shown_cur = -2;
    s_shown_state = -1;
    s_rows.clear();
    s_rows_built = false;
    s_seen_version = 0;
    s_cur_duration_ms = 0;

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
    const int root_w = lv_obj_get_width(root);
    const int root_h = lv_obj_get_height(root);

    /* now playing: artwork, title, artist, progress, controls */
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, root_w - 24, CARD_H - 16);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* artwork placeholder, as in esp_chatbot_v1: a gradient with two discs */
    s_art = lv_obj_create(card);
    lv_obj_remove_style_all(s_art);
    lv_obj_set_size(s_art, ART, ART);
    lv_obj_align(s_art, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_radius(s_art, 22, 0);
    lv_obj_set_style_clip_corner(s_art, true, 0);
    lv_obj_set_style_bg_opa(s_art, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(s_art, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(s_art, LV_OBJ_FLAG_SCROLLABLE);
    /* discs, note and cover in one layer, hidden as a whole while the spectrum shows: the artwork
     * does not clip then, and their square corners would show around the spectrum */
    s_art_content = lv_obj_create(s_art);
    lv_obj_remove_style_all(s_art_content);
    lv_obj_set_size(s_art_content, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_art_content, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    s_art_primary = lv_obj_create(s_art_content);
    lv_obj_remove_style_all(s_art_primary);
    lv_obj_set_size(s_art_primary, ART * 70 / 100, ART * 70 / 100);
    lv_obj_set_pos(s_art_primary, -ART * 17 / 100, ART * 52 / 100);
    lv_obj_set_style_radius(s_art_primary, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_art_primary, LV_OPA_70, 0);
    lv_obj_remove_flag(s_art_primary, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    s_art_secondary = lv_obj_create(s_art_content);
    lv_obj_remove_style_all(s_art_secondary);
    lv_obj_set_size(s_art_secondary, ART * 56 / 100, ART * 56 / 100);
    lv_obj_set_pos(s_art_secondary, ART * 59 / 100, -ART * 14 / 100);
    lv_obj_set_style_radius(s_art_secondary, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_art_secondary, LV_OPA_60, 0);
    lv_obj_remove_flag(s_art_secondary, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t *note = lv_label_create(s_art_content);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_opa(note, LV_OPA_80, 0);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_align(note, LV_ALIGN_BOTTOM_RIGHT, -14, -10);
    set_artwork(PALETTE_IDLE);
    /* the cover of the tag, over the placeholder */
    s_cover_img = lv_image_create(s_art_content);
    lv_obj_center(s_cover_img);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    /* the spectrum, over the cover; a tap on the artwork switches between them */
    s_viz = lv_obj_create(s_art);
    lv_obj_remove_style_all(s_viz);
    lv_obj_set_size(s_viz, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_viz, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_viz, lv_color_hex(0x171C24), 0);
    lv_obj_set_style_radius(s_viz, 22, 0);      /* same corners as the artwork, without clipping */
    lv_obj_set_style_pad_hor(s_viz, 12, 0);
    lv_obj_set_style_pad_ver(s_viz, 14, 0);
    lv_obj_remove_flag(s_viz, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    spectrum::view_create(s_viz_view, s_viz, 3, 4, 3);
    s_show_viz = phone_pref_get("mus_viz", 0) != 0;
    show_viz_mode();
    lv_obj_add_flag(s_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_art, on_art, LV_EVENT_SHORT_CLICKED, nullptr);

    /* mute badge, over everything in the artwork (shown only, a tap still switches the view) */
    s_mute_badge = lv_obj_create(s_art);
    lv_obj_remove_style_all(s_mute_badge);
    lv_obj_set_size(s_mute_badge, 72, 72);
    lv_obj_center(s_mute_badge);
    lv_obj_set_style_radius(s_mute_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mute_badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mute_badge, LV_OPA_70, 0);
    lv_obj_remove_flag(s_mute_badge, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(s_mute_badge, LV_OBJ_FLAG_HIDDEN);
    s_mute_icon = phone_ui_mute_icon(s_mute_badge, 48, lv_color_white());   /* crossed speaker */
    lv_obj_center(s_mute_icon);

    s_title = lv_label_create(card);
    lv_obj_set_style_text_font(s_title, phone_font_by_size(24), 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title, root_w - 72);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, ART + 28);
    lv_label_set_text(s_title, T("Chưa phát", "Not playing"));

    s_artist = lv_label_create(card);
    lv_obj_set_style_text_color(s_artist, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_artist, phone_font_by_size(18), 0);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_artist, root_w - 72);
    lv_obj_set_height(s_artist, lv_font_get_line_height(phone_font_by_size(18)));
    lv_obj_set_style_text_align(s_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_artist, LV_ALIGN_TOP_MID, 0, ART + 62);
    lv_label_set_text(s_artist, "");

    /* elapsed, progress, duration on one line */
    const int line_y = ART + 100;
    s_time = lv_label_create(card);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_time, phone_font_by_size(16), 0);
    lv_obj_set_width(s_time, 64);       /* h:mm:ss */
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_LEFT, 8, line_y);
    lv_label_set_text(s_time, "0:00");
    s_duration = lv_label_create(card);
    lv_obj_set_style_text_color(s_duration, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_duration, phone_font_by_size(16), 0);
    lv_obj_set_width(s_duration, 64);
    lv_obj_align(s_duration, LV_ALIGN_TOP_RIGHT, -8, line_y);
    lv_label_set_text(s_duration, "--:--");
    /* progress, dragged to jump (MP3, AAC, WAV, FLAC); the knob shows only while it can be dragged */
    s_dragging = false;
    s_bar = lv_slider_create(card);
    lv_obj_set_size(s_bar, root_w - 24 - 2 * 84, 6);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, line_y + 8);
    lv_slider_set_range(s_bar, 0, 1000);
    lv_obj_set_ext_click_area(s_bar, 16);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x3A444E), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_bar, 5, LV_PART_KNOB);
    lv_obj_set_style_opa(s_bar, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_add_state(s_bar, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_bar, on_seek, LV_EVENT_PRESS_LOST, nullptr);

    /* repeat, previous, play / pause, next */
    lv_obj_align(round_button(card, LV_SYMBOL_PREV, 60, 0x2A333C, on_prev), LV_ALIGN_BOTTOM_MID, -92, -22);
    lv_obj_align(round_button(card, LV_SYMBOL_PLAY, 76, COLOR_ACCENT, on_play, &s_play_label), LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_align(round_button(card, LV_SYMBOL_NEXT, 60, 0x2A333C, on_next), LV_ALIGN_BOTTOM_MID, 92, -22);

    lv_obj_t *repeat = lv_button_create(card);
    lv_obj_remove_style_all(repeat);
    lv_obj_set_size(repeat, 64, 48);
    lv_obj_set_ext_click_area(repeat, 8);
    lv_obj_align(repeat, LV_ALIGN_BOTTOM_LEFT, 10, -28);
    lv_obj_add_event_cb(repeat, on_repeat, LV_EVENT_CLICKED, nullptr);
    s_repeat_label = lv_label_create(repeat);
    lv_label_set_text(s_repeat_label, LV_SYMBOL_LOOP);
    lv_obj_align(s_repeat_label, LV_ALIGN_LEFT_MID, 8, 0);
    s_repeat_tag = lv_label_create(repeat);
    lv_obj_set_style_text_font(s_repeat_tag, phone_font_by_size(12), 0);
    lv_label_set_text(s_repeat_tag, "");
    lv_obj_align(s_repeat_tag, LV_ALIGN_LEFT_MID, 38, 1);
    show_repeat();

    /* shuffle, opposite the repeat button */
    lv_obj_t *shuffle = lv_button_create(card);
    lv_obj_remove_style_all(shuffle);
    lv_obj_set_size(shuffle, 64, 48);
    lv_obj_set_ext_click_area(shuffle, 8);
    lv_obj_align(shuffle, LV_ALIGN_BOTTOM_RIGHT, -10, -28);
    lv_obj_set_style_opa(shuffle, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(shuffle, on_shuffle, LV_EVENT_CLICKED, nullptr);
    s_shuffle_label = lv_label_create(shuffle);
    lv_label_set_text(s_shuffle_label, LV_SYMBOL_SHUFFLE);
    lv_obj_center(s_shuffle_label);
    show_shuffle();

    /* list filter: all tracks or the favorites only */
    lv_obj_t *filter = lv_button_create(card);
    lv_obj_remove_style_all(filter);
    lv_obj_set_size(filter, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_pad_hor(filter, 10, 0);
    lv_obj_set_ext_click_area(filter, 6);
    lv_obj_align(filter, LV_ALIGN_TOP_LEFT, 6, 8);
    lv_obj_set_style_opa(filter, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_event_cb(filter, on_filter, LV_EVENT_CLICKED, nullptr);
    s_filter_label = lv_label_create(filter);
    lv_obj_set_style_text_font(s_filter_label, phone_font_by_size(16), 0);
    lv_obj_center(s_filter_label);
    show_filter();

    /* track list; it ends above the strip where the swipe-up for the navigation bar starts */
    s_list = lv_obj_create(root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, root_w, root_h - CARD_H - 36);
    lv_obj_set_y(s_list, CARD_H);
    lv_obj_set_style_pad_hor(s_list, 12, 0);
    lv_obj_set_style_pad_bottom(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, ROW_GAP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    phone_ui_scrollbar(s_list);
    lv_obj_add_event_cb(s_list, on_scroll, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(s_list, on_scroll, LV_EVENT_SCROLL_END, nullptr);

    s_empty = lv_label_create(root);
    lv_obj_set_style_text_color(s_empty, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, CARD_H + 70);
    lv_label_set_text(s_empty, phone_storage_get(nullptr, nullptr) == PHONE_STORAGE_MOUNTED
                      ? T("Đang tìm nhạc...", "Looking for music...") : T("Không có thẻ nhớ", "No microSD card"));
}

} // namespace

namespace esp_brookesia::apps {

PhoneMusic *PhoneMusic::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PhoneMusic();
    }
    return _instance;
}

PhoneMusic::PhoneMusic():
    App(phone_tr("Nhạc", "Music"), &phone_music_icon, true, true, true)
{
}

PhoneMusic::~PhoneMusic()
{
}

bool PhoneMusic::run()
{
    s_hold_name = phone_tr("Nhạc", "Music");
    phone_storage_hold(s_hold_name);      /* files stay open in the background */
    stop_playback();
    s_cur = -1;
    s_error = false;
    const int repeat = phone_pref_get("mus_rep", REPEAT_OFF);
    s_repeat = repeat >= 0 && repeat < REPEAT_COUNT ? repeat : REPEAT_OFF;
    s_only_fav = phone_pref_get("mus_fav", 0) != 0;
    s_shuffle = phone_pref_get("mus_shuf", 0) != 0;
    load_favorites();
    {
        std::lock_guard<std::mutex> lock(s_order_mtx);
        s_order.clear();
        s_play.clear();
    }
    s_row_of.clear();
    retire_library();
    start_library();
    s_cover_buf_size = (static_cast<size_t>(ART) * ART * 2 + COVER_ALIGN - 1) & ~static_cast<size_t>(COVER_ALIGN - 1);
    for (auto &buf : s_cover_buf) {
        if (!buf) {
            buf = static_cast<uint8_t *>(heap_caps_aligned_calloc(COVER_ALIGN, 1, s_cover_buf_size, MALLOC_CAP_SPIRAM));
        }
    }
    s_cover_front = -1;
    s_cover_ready = COVER_NONE;
    build_ui();
    lv_timer_create(on_timer, 250, nullptr);
    lv_timer_create(on_viz_timer, 40, nullptr);
    return true;
}

bool PhoneMusic::back()
{
    return notifyCoreClosed();
}

/* Leaving to the home screen keeps the music playing; closing the app stops it */
bool PhoneMusic::keepRunning() const
{
    return s_playing;
}

bool PhoneMusic::close()
{
    s_viz_feed = false;
    end_player();
    retire_library();
    phone_storage_release(s_hold_name);
    /* the image still points at a cover buffer until the core deletes the screen */
    if (s_root && s_cover_img) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_cover_img, nullptr);
    }
    for (auto &buf : s_cover_buf) {
        heap_caps_free(buf);
        buf = nullptr;
    }
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PhoneMusic, APP_NAME, []()
{
    return std::shared_ptr<PhoneMusic>(PhoneMusic::requestInstance(), [](PhoneMusic * p) {});
})

} // namespace esp_brookesia::apps
