/*
 * Radio player: one play task runs esp_audio_simple_player_run_to_end() on the station URL and retries;
 * stop requests interrupt it with esp_audio_simple_player_stop() from a short worker task, so that the
 * LVGL task never waits on the pipeline. The output callback (in the player's own task) opens the codec
 * at the stream rate, mixes to mono, writes to the speaker and feeds the spectrum task.
 */
#include "radio_player.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <strings.h>
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_fourcc.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_hls_io.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "media_lib_adapter.h"
#include "phone_audio.h"
#include "phone_net.h"
#include "phone_spectrum.hpp"

namespace radio {

namespace {

const char *TAG = "radio";
constexpr int MAX_CONNECT_FAILURES = 3;     // attempts in a row without sound before giving up
constexpr uint32_t RETRY_MS = 1200;
constexpr uint32_t NET_WAIT_MS = 120000;     // no Internet (Wi-Fi lost, 4G not up yet): wait this long for it

std::mutex s_mtx;                           // s_url
std::string s_url;
std::atomic<uint32_t> s_gen;                // station changes
std::atomic<bool> s_want;                   // the user wants sound
std::atomic<bool> s_quit;
std::atomic<bool> s_in_run;                 // inside run_to_end
std::atomic<bool> s_stop_busy;              // a stop worker is running
std::atomic<bool> s_heard;                  // sound came out in this run
std::atomic<int> s_status{static_cast<int>(Status::STOPPED)};
std::atomic<bool> s_speaker_busy;
std::atomic<TaskHandle_t> s_task;
std::mutex s_life_mtx;                      // a task ending vs. ensure_tasks() starting it again
esp_asp_handle_t s_player;                  // play task only (and the stop worker while s_stop_busy)

/* stream format, from the MUSIC_INFO event and the HLS segment callback */
std::atomic<int> s_rate;
std::atomic<int> s_channels{2};
std::atomic<uint32_t> s_seg_format;
uint32_t s_hls_last_format;

/* output, in the player task */
esp_codec_dev_handle_t s_spk;               // taken by the play task; given back by it or by a yield
std::mutex s_spk_mtx;                       // s_spk and the codec: output callback, play task, yield
int s_open_rate;
int s_volume = -1;
int16_t *s_mono;
size_t s_mono_cap;

void set_status(Status st)
{
    s_status = static_cast<int>(st);
}

/* True when the task may end: the app was not opened again meanwhile (else s_task stays and it goes on) */
bool may_end(std::atomic<TaskHandle_t> &slot)
{
    std::lock_guard<std::mutex> lk(s_life_mtx);
    if (!s_quit) {
        return false;
    }
    slot = nullptr;
    return true;
}

/* ---- player ---- */

/* With s_spk_mtx held */
void close_codec_locked()
{
    if (s_spk && s_open_rate) {
        esp_codec_dev_close(s_spk);
    }
    s_open_rate = 0;
}

void close_codec()
{
    std::lock_guard<std::mutex> lk(s_spk_mtx);
    close_codec_locked();
}

int on_output(uint8_t *data, int size, void *)
{
    if (!s_want || s_quit || size <= 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(s_spk_mtx);
    if (!s_spk) {
        return 0;                           /* given away to another player: the pipeline is stopping */
    }
    const int rate = s_rate;
    const int ch = std::max(1, s_channels.load());
    if (rate <= 0) {
        return 0;
    }
    if (rate != s_open_rate) {
        close_codec_locked();
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = rate;
        fs.channel = 1;
        fs.bits_per_sample = 16;
        if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec open %d Hz failed", rate);
            return 0;
        }
        s_open_rate = rate;
        s_volume = -1;
    }
    const int frames = size / 2 / ch;
    if (static_cast<size_t>(frames) > s_mono_cap) {
        heap_caps_free(s_mono);
        s_mono = static_cast<int16_t *>(heap_caps_malloc(frames * 2, MALLOC_CAP_SPIRAM));
        s_mono_cap = s_mono ? frames : 0;
        if (!s_mono) {
            return 0;
        }
    }
    const auto *pcm = reinterpret_cast<const int16_t *>(data);
    for (int i = 0; i < frames; i++) {
        int sum = 0;
        for (int c = 0; c < ch; c++) {
            sum += pcm[i * ch + c];
        }
        s_mono[i] = static_cast<int16_t>(sum / ch);
    }
    const int v = phone_audio_get_volume();
    if (v != s_volume) {
        s_volume = v;
        esp_codec_dev_set_out_vol(s_spk, v);
    }
    esp_codec_dev_write(s_spk, s_mono, frames * 2);
    spectrum::feed(s_mono, frames, rate);
    if (!s_heard.exchange(true)) {
        set_status(Status::PLAYING);
    }
    return 0;
}

int on_event(esp_asp_event_pkt_t *ev, void *)
{
    if (ev && ev->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && ev->payload_size >= static_cast<int>(sizeof(esp_asp_music_info_t))) {
        esp_asp_music_info_t info = {};
        memcpy(&info, ev->payload, sizeof(info));
        if (info.channels > 0) {
            s_channels = info.channels;
        }
        if (info.sample_rate > 0) {
            s_rate = info.sample_rate;
        }
        ESP_LOGI(TAG, "stream: %d Hz, %d ch, %d bit", info.sample_rate, info.channels, info.bits);
    }
    return 0;
}

/* Each HLS segment: the decoder follows its format (AAC, MP3, TS) */
int on_segment(esp_hls_file_seg_info_t *info, void *ctx)
{
    if (!info || !ctx || s_hls_last_format == info->format) {
        return 0;
    }
    s_hls_last_format = info->format;
    s_seg_format = info->format;
    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_element_handle_t dec = nullptr;
    if (esp_audio_simple_player_get_pipeline(static_cast<esp_asp_handle_t>(ctx), &pipe) != ESP_GMF_ERR_OK || !pipe ||
            esp_gmf_pipeline_get_el_by_name(pipe, "aud_dec", &dec) != ESP_GMF_ERR_OK || !dec) {
        return 0;
    }
    esp_gmf_info_sound_t snd = {};
    snd.format_id = info->format;
    snd.sample_rates = 44100;
    snd.channels = 2;
    snd.bits = 16;
    esp_gmf_audio_dec_reconfig_by_sound_info(dec, &snd);
    ESP_LOGI(TAG, "HLS segments: %s", ESP_FOURCC_TO_STR(info->format));
    return 0;
}

/* ---- ICY metadata (Icecast / Shoutcast streams) ----
 * With "Icy-MetaData: 1" in the request, the server answers "icy-metaint: N" and puts a metadata block
 * after every N bytes of sound: one length byte L, then L * 16 bytes of text, for example
 * "StreamTitle='Artist - Song';". The read hook takes the blocks out, so the decoder only gets sound,
 * and keeps the title. All in the HTTP reader task, except title(). */

struct Icy {
    int metaint;                // 0: no metadata in this stream
    int to_meta;                // sound bytes left before the next length byte
    int meta_left;              // text bytes left in the current block
    int meta_len;
    char meta[16 * 255 + 1];
};
EXT_RAM_BSS_ATTR Icy s_icy;          // 4 KB, in PSRAM
std::mutex s_title_mtx;
std::string s_title;

/* ISO-8859-1 text (older servers) to UTF-8; UTF-8 text unchanged */
std::string to_utf8(const char *p, size_t n)
{
    bool utf8 = true;
    for (size_t i = 0; i < n && utf8;) {
        const uint8_t c = p[i];
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) {
            utf8 = false;
            break;
        }
        for (int k = 1; k < len; k++) {
            utf8 = utf8 && (static_cast<uint8_t>(p[i + k]) >> 6) == 2;
        }
        i += len;
    }
    if (utf8) {
        return std::string(p, n);
    }
    std::string out;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void icy_block_done()
{
    s_icy.meta[s_icy.meta_len] = '\0';
    const char *t = strstr(s_icy.meta, "StreamTitle='");
    if (!t) {
        return;
    }
    t += 13;
    const char *end = strstr(t, "';");
    const size_t n = end ? static_cast<size_t>(end - t) : strlen(t);
    std::string title = to_utf8(t, n);
    std::lock_guard<std::mutex> lk(s_title_mtx);
    if (title != s_title) {
        s_title = title;
        ESP_LOGI(TAG, "now playing: %s", s_title.c_str());
    }
}

/* Takes the metadata out of buf[0..n): returns the sound bytes left at the front of buf */
int icy_filter(char *buf, int n)
{
    int out = 0;
    for (int i = 0; i < n;) {
        if (s_icy.meta_left > 0) {
            const int k = std::min(n - i, s_icy.meta_left);
            memcpy(s_icy.meta + s_icy.meta_len, buf + i, k);
            s_icy.meta_len += k;
            s_icy.meta_left -= k;
            i += k;
            if (s_icy.meta_left == 0) {
                icy_block_done();
                s_icy.to_meta = s_icy.metaint;
            }
        } else if (s_icy.to_meta == 0) {
            const int len = static_cast<uint8_t>(buf[i++]) * 16;      /* the length byte */
            s_icy.meta_len = 0;
            s_icy.meta_left = len;
            if (len == 0) {
                s_icy.to_meta = s_icy.metaint;                     /* no change of title */
            }
        } else {
            const int k = std::min(n - i, s_icy.to_meta);
            memmove(buf + out, buf + i, k);
            out += k;
            s_icy.to_meta -= k;
            i += k;
        }
    }
    return out;
}

/* Hook of the HTTP reader (io_http_tls): direct streams, also used by HLS for its segments */
int http_hook(http_stream_event_msg_t *msg)
{
    auto *client = static_cast<esp_http_client_handle_t>(msg->http_client);
    switch (msg->event_id) {
    case HTTP_STREAM_PRE_REQUEST:
        s_icy.metaint = 0;
        s_icy.meta_left = 0;
        esp_http_client_set_header(client, "Icy-MetaData", "1");
        return 0;
    case HTTP_STREAM_ON_HEADER: {
        const auto *evt = static_cast<const esp_http_client_event_t *>(msg->buffer);
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            const int m = atoi(evt->header_value);
            if (m > 0 && m <= 1024 * 1024) {
                s_icy.metaint = m;
                s_icy.to_meta = m;
                ESP_LOGI(TAG, "ICY metadata every %d bytes", m);
            }
        } else if (strcasecmp(evt->header_key, "Content-Type") == 0 && !s_seg_format) {
            char v[48];
            snprintf(v, sizeof(v), "%s", evt->header_value);
            for (char *c = v; *c; c++) {
                *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
            }
            /* the format line of direct streams (HLS sets it from its segments) */
            if (strstr(v, "mpeg") && !strstr(v, "mpegurl")) {
                s_seg_format = ESP_FOURCC_MP3;
            } else if (strstr(v, "aac")) {
                s_seg_format = ESP_FOURCC_AAC;
            } else if (strstr(v, "ogg")) {
                s_seg_format = ESP_FOURCC_OGG;
            } else if (strstr(v, "flac")) {
                s_seg_format = ESP_FOURCC_FLAC;
            }
        }
        return 0;
    }
    case HTTP_STREAM_ON_RESPONSE: {
        if (s_icy.metaint == 0) {
            return 0;                       /* plain read by the reader */
        }
        /* read and strip until some sound is left (a read may hold metadata only) */
        for (;;) {
            const int r = esp_http_client_read(client, static_cast<char *>(msg->buffer), msg->buffer_len);
            if (r <= 0) {
                return r < 0 ? -1 : 0;      /* 0: the reader reads again and sees the end */
            }
            const int out = icy_filter(static_cast<char *>(msg->buffer), r);
            if (out > 0) {
                return out;
            }
        }
    }
    default:
        return 0;
    }
}

/* The pool has a plain "io_http" already; HLS fetches over https need the certificate bundle, so this
 * one has its own tag and scores higher on http(s) URLs that are not playlists */
esp_gmf_err_t http_score(esp_gmf_io_handle_t, const char *url, int *score)
{
    *score = ESP_GMF_IO_SCORE_NONE;
    if (!url) {
        return ESP_GMF_ERR_OK;
    }
    const char *file = strrchr(url, '/');
    file = file ? file + 1 : url;
    if (strstr(file, ".m3u8") || strstr(file, ".M3U8")) {
        return ESP_GMF_ERR_OK;
    }
    if (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0) {
        *score = ESP_GMF_IO_SCORE_STANDARD + 10;
    }
    return ESP_GMF_ERR_OK;
}

void destroy_player()
{
    if (s_player) {
        esp_audio_simple_player_stop(s_player);
        esp_audio_simple_player_destroy(s_player);
        s_player = nullptr;
    }
}

bool create_player()
{
    static bool s_media_lib;
    if (!s_media_lib) {
        media_lib_add_default_adapter();
        s_media_lib = true;
    }
    /* GMF logs several lines per segment */
    for (const char *tag : {"ESP_GMF_TASK", "ESP_GMF_BLOCK", "ESP_GMF_PORT", "NEW_DATA_BUS", "ESP_GMF_POOL", "ESP_GMF_EL",
                            "ESP_ES_PARSER", "AUD_SIMP_PLAYER", "HLS_Playlist", "esp-x509-crt-bundle", "ESP_GMF_ADEC",
                            "ESP_GMF_HTTP", "HTTP_CLIENT"
                           }) {
        esp_log_level_set(tag, ESP_LOG_WARN);
    }
    esp_asp_cfg_t cfg = {};
    cfg.out.cb = on_output;
    cfg.task_prio = 5;
    cfg.task_stack = 8 * 1024;
    cfg.task_core = 1;                      /* away from the LVGL task */
    cfg.task_stack_in_ext = true;
    if (esp_audio_simple_player_new(&cfg, &s_player) != ESP_GMF_ERR_OK || !s_player) {
        s_player = nullptr;
        return false;
    }
    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_audio_simple_player_get_pool(s_player, &pool) != ESP_GMF_ERR_OK || !pool) {
        destroy_player();
        return false;
    }
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.event_handle = http_hook;
    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) == ESP_GMF_ERR_OK && http_io) {
        reinterpret_cast<esp_gmf_io_t *>(http_io)->get_score = http_score;
        esp_gmf_obj_set_tag(reinterpret_cast<esp_gmf_obj_handle_t>(http_io), "io_http_tls");
        if (esp_audio_simple_player_register_io(s_player, http_io) != ESP_GMF_ERR_OK) {
            esp_gmf_obj_delete(http_io);
        }
    }
    esp_hls_io_cfg_t hls_cfg = {};
    hls_cfg.name = "io_hls";
    hls_cfg.file_seg_cb = on_segment;
    hls_cfg.ctx = s_player;
    hls_cfg.pool = pool;
    hls_cfg.io_cfg = DEFAULT_HLS_IO_CFG();
    esp_gmf_io_handle_t hls_io = nullptr;
    if (esp_gmf_io_hls_init(&hls_cfg, &hls_io) != ESP_GMF_ERR_OK || !hls_io) {
        destroy_player();
        return false;
    }
    if (esp_audio_simple_player_register_io(s_player, hls_io) != ESP_GMF_ERR_OK) {
        esp_gmf_obj_delete(hls_io);
        destroy_player();
        return false;
    }
    esp_audio_simple_player_set_event(s_player, on_event, nullptr);
    return true;
}

/* Any task: closes the codec and gives the speaker back at once, even while the pipeline still runs
 * (the output callback finds no speaker and drops the sound) */
void release_speaker()
{
    std::lock_guard<std::mutex> lk(s_spk_mtx);
    close_codec_locked();
    if (s_spk) {
        s_spk = nullptr;
        phone_audio_speaker_give();
    }
}

/* Interrupts run_to_end() without making the caller wait for the pipeline */
void interrupt()
{
    if (!s_in_run || s_stop_busy.exchange(true)) {
        return;
    }
    auto worker = [](void *) {
        if (s_player) {
            esp_audio_simple_player_stop(s_player);
        }
        s_stop_busy = false;
        vTaskDeleteWithCaps(nullptr);
    };
    if (xTaskCreatePinnedToCoreWithCaps(worker, "radio_stop", 4096, nullptr, 6, nullptr, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_stop_busy = false;
    }
}

/* Without Internet an attempt only fails: wait for Wi-Fi or 4G instead (not counted as a failure), and start
 * again as soon as either is up. False when it did not come back, or the radio was stopped or changed meanwhile. */
bool wait_for_internet(uint32_t gen)
{
    if (phone_net_online()) {
        return true;
    }
    ESP_LOGW(TAG, "no Internet: waiting for Wi-Fi or 4G");
    set_status(Status::RETRYING);
    const int64_t end = esp_timer_get_time() + static_cast<int64_t>(NET_WAIT_MS) * 1000;
    while (!s_quit && s_want && s_gen == gen && esp_timer_get_time() < end) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300));
        if (phone_net_online()) {
            ESP_LOGI(TAG, "Internet is back: play again");
            return true;
        }
    }
    return false;
}

void play_task(void *)
{
    int failures = 0;
    uint32_t failure_gen = s_gen;
again:
    while (!s_quit) {
        if (!s_want) {
            release_speaker();
            if (s_status != static_cast<int>(Status::FAILED)) {
                set_status(Status::STOPPED);
            }
            failures = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_spk) {
            /* another player taking the speaker (music, video, assistant) stops the radio */
            /* the GMF pipeline can take seconds to stop (stuck in an HTTP open): the speaker is given back
             * first, the pipeline stops after */
            auto *spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take_yielding([]() {
                ESP_LOGI(TAG, "another player takes the speaker: stop");
                s_want = false;
                release_speaker();
                stop();
            }));
            {
                std::lock_guard<std::mutex> lk(s_spk_mtx);
                s_spk = spk;
            }
            if (!s_spk) {
                ESP_LOGW(TAG, "speaker busy");
                s_speaker_busy = true;
                set_status(Status::FAILED);
                s_want = false;
                continue;
            }
        }
        if (!s_player && !create_player()) {
            ESP_LOGE(TAG, "cannot create the player");
            set_status(Status::FAILED);
            s_want = false;
            continue;
        }
        const uint32_t gen = s_gen;
        if (gen != failure_gen) {
            failure_gen = gen;
            failures = 0;
        }
        std::string url;
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            url = s_url;
        }
        s_heard = false;
        s_hls_last_format = 0;
        s_speaker_busy = false;
        if (!wait_for_internet(gen)) {
            if (s_want && s_gen == gen && !s_quit) {
                ESP_LOGW(TAG, "no Internet for %u s: give up", static_cast<unsigned>(NET_WAIT_MS / 1000));
                set_status(Status::FAILED);
                s_want = false;
            }
            continue;
        }
        set_status(failures ? Status::RETRYING : Status::CONNECTING);
        ESP_LOGI(TAG, "play %s", url.c_str());
        s_in_run = true;
        if (s_quit || !s_want || s_gen != gen) {
            s_in_run = false;               /* changed while the URL was read */
            continue;
        }
        const esp_gmf_err_t err = esp_audio_simple_player_run_to_end(s_player, url.c_str(), nullptr);
        s_in_run = false;
        while (s_stop_busy) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        close_codec();
        if (s_quit || !s_want || s_gen != gen) {
            continue;                       /* stopped, or another station */
        }
        if ((err != ESP_GMF_ERR_OK || !s_heard) && !phone_net_online()) {
            continue;                       /* the network went away: wait for it above, not a failure */
        }
        if (err != ESP_GMF_ERR_OK || !s_heard) {
            failures = s_heard ? 0 : failures + 1;   /* a drop while playing is not a failure to connect */
            ESP_LOGW(TAG, "stream ended (0x%x), %d attempts without sound", err, failures);
            if (failures >= MAX_CONNECT_FAILURES) {
                set_status(Status::FAILED);
                s_want = false;
                continue;
            }
            set_status(Status::RETRYING);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));  /* the stream ended cleanly: open it again */
        }
    }
    destroy_player();
    release_speaker();
    heap_caps_free(s_mono);
    s_mono = nullptr;
    s_mono_cap = 0;
    set_status(Status::STOPPED);
    if (!may_end(s_task)) {
        goto again;
    }
    vTaskDeleteWithCaps(nullptr);
}

void ensure_tasks()
{
    std::lock_guard<std::mutex> lk(s_life_mtx);
    s_quit = false;
    if (!s_task) {
        TaskHandle_t t = nullptr;
        if (xTaskCreatePinnedToCoreWithCaps(play_task, "radio_play", 6144, nullptr, 5, &t, tskNO_AFFINITY,
                                            MALLOC_CAP_SPIRAM) == pdPASS) {
            s_task = t;
        }
    }
}

} // namespace

void play(const std::string &url)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_url = url;
    }
    s_gen++;
    s_rate = 0;
    s_seg_format = 0;
    {
        std::lock_guard<std::mutex> lk(s_title_mtx);
        s_title.clear();
    }
    s_want = true;
    set_status(Status::CONNECTING);
    ensure_tasks();
    interrupt();                            /* another station while one plays */
    if (TaskHandle_t t = s_task) {
        xTaskNotifyGive(t);
    }
}

void stop()
{
    s_want = false;
    interrupt();
    if (TaskHandle_t t = s_task) {
        xTaskNotifyGive(t);
    }
}

void shutdown()
{
    s_want = false;
    s_quit = true;
    release_speaker();                      /* at once: the pipeline may take seconds to stop */
    interrupt();
    if (TaskHandle_t t = s_task) {
        xTaskNotifyGive(t);
    }
    /* No wait: this runs in the LVGL task (app closed), and the GMF pipeline can take seconds to stop
     * (stuck in an HTTP open). The speaker is already given back; the tasks end and free the player on
     * their own. Opening the app again meanwhile reuses the play task (ensure_tasks clears s_quit). */
    for (int i = 0; i < 10 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

Status status()
{
    return static_cast<Status>(s_status.load());
}

bool busy_speaker()
{
    return s_speaker_busy;
}

void format(char *buf, size_t len)
{
    const int rate = s_rate;
    const uint32_t fmt = s_seg_format;
    if (rate <= 0) {
        buf[0] = '\0';
        return;
    }
    const char *name = fmt ? ESP_FOURCC_TO_STR(fmt) : "";
    snprintf(buf, len, "%s%s%d.%d kHz %s", name, fmt ? " · " : "", rate / 1000, rate % 1000 / 100,
             s_channels >= 2 ? "stereo" : "mono");
}

void title(char *buf, size_t len)
{
    std::lock_guard<std::mutex> lk(s_title_mtx);
    snprintf(buf, len, "%s", s_title.c_str());
}

} // namespace radio
