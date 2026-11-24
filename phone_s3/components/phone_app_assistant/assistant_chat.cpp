/*
 * Voice session of the assistant.
 *
 * Two tasks:
 *   - the session task owns the esp_xiaozhi chat handle: it runs the commands (button, server
 *     events) and, while a conversation is open, reads the microphone in 60 ms frames, encodes
 *     them with Opus and sends them while the assistant listens;
 *   - the play task decodes the Opus packets of the answer and writes them to the speaker.
 *
 * The conversation is half duplex: nothing is sent while the answer plays, so no echo
 * cancellation is needed. After the answer, the assistant listens again by itself ("auto" mode of
 * the protocol) until the button ends the conversation or the server says goodbye.
 *
 * The microphone and the speaker share the I2S clock, so both run at 16 kHz; the Opus decoder
 * resamples the answer, whatever rate the server encodes it at.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "assistant_afe.h"
#include "assistant_mcp.h"
#include "assistant_wake.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "phone_audio.h"
#include "phone_locale.h"
#include "assistant_chat.h"

namespace {

#define T(vi, en) phone_tr(vi, en)

const char *TAG = "assistant_chat";

constexpr int RATE = 16000;
constexpr int FRAME_MS = 60;
constexpr int FRAME_SAMPLES = RATE * FRAME_MS / 1000;
constexpr int OPUS_BITRATE = 24000;
constexpr float MIC_GAIN_DB = 30.0f;
constexpr int AUDIO_QUEUE_LEN = 80;         // about 5 s of answer waiting to be played
constexpr size_t PCM_BYTES = 8192;          // one decoded packet (up to 120 ms at 16 kHz, with room)

enum Cmd {
    CMD_BUTTON, CMD_CONNECTED, CMD_DISCONNECTED, CMD_CHANNEL_OPENED, CMD_CHANNEL_CLOSED, CMD_GOODBYE,
    CMD_TTS_START, CMD_TTS_STOP, CMD_OPTIONS, CMD_CALIBRATE, CMD_STOP,
};

struct Packet {
    uint8_t *data;
    int len;
};

std::atomic<int> s_state{ASSISTANT_CHAT_OFF};
std::atomic<int> s_mic_level;
std::atomic<int> s_queued;                  // packets waiting in s_audio or being played
QueueHandle_t s_cmds;
QueueHandle_t s_msgs;
QueueHandle_t s_audio;
SemaphoreHandle_t s_done;
SemaphoreHandle_t s_spk_lock;
TaskHandle_t s_session;
TaskHandle_t s_player;
std::atomic<bool> s_player_run;
std::atomic<bool> s_player_done;
bool s_has_mqtt, s_has_websocket;

/* session task only */
esp_xiaozhi_chat_handle_t s_chat;
esp_codec_dev_handle_t s_spk;               // also read by the play task, under s_spk_lock
esp_codec_dev_handle_t s_mic;
bool s_spk_open, s_mic_open;
bool s_talking;                             // a conversation is open: codecs are open, the microphone is read
bool s_answer_done;                         // the server finished the answer; listen again once it has been played
bool s_goodbye;                             // the server ended the conversation; stop once the answer has been played
bool s_connected;                           // link to the server, as last reported
bool s_wake;                                // the wake word model is loaded; the idle microphone listens for it
bool s_options_changed;                     // a switch changed: models opened or closed when no conversation is open
bool s_calibrate;                           // measure the echo delay once no conversation is open
bool s_wake_want = true, s_barge_want = true;   // the switches, as they are now (a switch turned off acts at once)
int16_t *s_wake_buf;
bool s_afe_on;                              // saying the wake word over the answer interrupts it (esp-sr front-end)
int16_t *s_ref_buf;                         // what the speaker played during the last microphone frame
int16_t *s_acc_mic, *s_acc_ref;             // microphone and reference waiting for a full front-end chunk
int s_acc_n;
void *s_enc;
int s_enc_in, s_enc_out;
uint8_t *s_enc_buf;
int16_t *s_mic_buf;

void post(Cmd cmd)
{
    if (s_cmds) {
        const int c = cmd;
        xQueueSend(s_cmds, &c, 0);
    }
}

void push_msg(assistant_chat_msg_type_t type, const char *text)
{
    if (!s_msgs || !text) {
        return;
    }
    auto *msg = static_cast<assistant_chat_msg_t *>(malloc(sizeof(assistant_chat_msg_t)));
    if (!msg) {
        return;
    }
    msg->type = type;
    strlcpy(msg->text, text, sizeof(msg->text));
    if (xQueueSend(s_msgs, &msg, 0) != pdPASS) {
        free(msg);
    }
}

void flush_audio()
{
    Packet p;
    while (s_audio && xQueueReceive(s_audio, &p, 0) == pdPASS) {
        free(p.data);
        s_queued--;
    }
}

/* ---- callbacks of esp_xiaozhi (its own tasks) ---- */

void on_audio(const uint8_t *data, int len, void *)
{
    if (len <= 0 || s_state != ASSISTANT_CHAT_SPEAKING) {
        return;
    }
    Packet p = {static_cast<uint8_t *>(malloc(len)), len};
    if (!p.data) {
        return;
    }
    memcpy(p.data, data, len);
    s_queued++;
    if (xQueueSend(s_audio, &p, 0) != pdPASS) {
        free(p.data);
        s_queued--;
    }
}

void on_chat_event(esp_xiaozhi_chat_event_t event, void *event_data, void *)
{
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        auto *text = static_cast<esp_xiaozhi_chat_text_data_t *>(event_data);
        if (text && text->text) {
            const bool user = text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER;
            push_msg(user ? ASSISTANT_CHAT_MSG_USER : ASSISTANT_CHAT_MSG_ASSISTANT, text->text);
            int listening = ASSISTANT_CHAT_LISTENING;
            if (user) {
                s_state.compare_exchange_strong(listening, ASSISTANT_CHAT_THINKING);
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI:
        push_msg(ASSISTANT_CHAT_MSG_EMOTION, static_cast<const char *>(event_data));
        break;
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        auto *tts = static_cast<esp_xiaozhi_chat_tts_state_t *>(event_data);
        if (tts && tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            post(CMD_TTS_START);
        } else if (tts && tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            post(CMD_TTS_STOP);
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR:
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Máy chủ báo lỗi", "The server reported an error"));
        break;
    default:
        break;
    }
}

void on_link_event(void *, esp_event_base_t, int32_t id, void *)
{
    switch (id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        post(CMD_CONNECTED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        post(CMD_DISCONNECTED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        post(CMD_CHANNEL_OPENED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        post(CMD_CHANNEL_CLOSED);
        break;
    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        post(CMD_GOODBYE);
        break;
    default:
        break;
    }
}

/* ---- play task ---- */

/* ---- software echo reference (stage 3b) ----
 * What the speaker plays is kept on the microphone's own clock: the ring holds, at index t, the sample that was
 * handed to the speaker when t microphone samples had been read. The echo in the microphone lags that by a
 * constant delay (output buffers, air, input buffers): 187-192 samples measured in stage 3b (pref "aecd"). */

constexpr int REF_RING = 16384;             // samples, a power of two
constexpr int REF_MASK = REF_RING - 1;
constexpr int REF_DELAY_DEFAULT = 187;
int16_t *s_ref_ring;                        // PSRAM
portMUX_TYPE s_ref_mux = portMUX_INITIALIZER_UNLOCKED;
int64_t s_mic_idx;                          // microphone samples read so far
int64_t s_mic_us;                           // esp_timer time when s_mic_idx last advanced
int64_t s_play_end;                         // timeline index where what was handed to the speaker ends
int s_ref_delay = REF_DELAY_DEFAULT;

/* caller holds s_ref_mux */
int64_t mic_now_locked()
{
    int64_t t = (esp_timer_get_time() - s_mic_us) * RATE / 1000000;
    t = t < 0 ? 0 : (t > RATE / 20 ? RATE / 20 : t);
    return s_mic_idx + t;
}

void ref_reset()
{
    taskENTER_CRITICAL(&s_ref_mux);
    if (s_ref_ring) {
        memset(s_ref_ring, 0, REF_RING * sizeof(int16_t));
    }
    s_mic_idx = 0;
    s_mic_us = esp_timer_get_time();
    s_play_end = 0;
    taskEXIT_CRITICAL(&s_ref_mux);
}

/* The speaker is about to play n samples; returns where they start on the timeline */
int64_t ref_put(const int16_t *pcm, int n)
{
    taskENTER_CRITICAL(&s_ref_mux);
    int64_t start = mic_now_locked();
    start = s_play_end > start ? s_play_end : start;
    if (s_ref_ring && pcm) {
        for (int k = 0; k < n; k++) {
            s_ref_ring[(start + k) & REF_MASK] = pcm[k];
        }
    }
    s_play_end = start + n;
    taskEXIT_CRITICAL(&s_ref_mux);
    return start;
}

/* n microphone samples were read: the matching playback comes out, then the clock advances */
void ref_take(int16_t *out, int n)
{
    taskENTER_CRITICAL(&s_ref_mux);
    for (int i = 0; i < n; i++) {
        const int64_t t = s_mic_idx + i - s_ref_delay;
        out[i] = s_ref_ring ? s_ref_ring[t & REF_MASK] : 0;
        if (s_ref_ring) {
            s_ref_ring[(t - REF_RING / 2) & REF_MASK] = 0;       // keep the old half of the ring clean
        }
    }
    s_mic_idx += n;
    s_mic_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_ref_mux);
}

void play_task(void *)
{
    esp_opus_dec_cfg_t cfg = {};
    cfg.sample_rate = RATE;
    cfg.channel = 1;
    cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    cfg.self_delimited = false;
    void *dec = nullptr;
    auto *pcm = static_cast<uint8_t *>(malloc(PCM_BYTES));
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK || !pcm) {
        ESP_LOGE(TAG, "no Opus decoder");
        dec = nullptr;
    }
    int volume = -1;
    while (s_player_run) {
        Packet p;
        if (xQueueReceive(s_audio, &p, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }
        if (dec) {
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = p.data;
            raw.len = p.len;
            esp_audio_dec_out_frame_t out = {};
            out.buffer = pcm;
            out.len = PCM_BYTES;
            esp_audio_dec_info_t info = {};
            if (esp_opus_dec_decode(dec, &raw, &out, &info) == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                xSemaphoreTake(s_spk_lock, portMAX_DELAY);
                if (s_spk_open) {
                    const int v = phone_audio_get_volume();
                    if (v != volume) {
                        volume = v;
                        esp_codec_dev_set_out_vol(s_spk, v);
                    }
                    ref_put(reinterpret_cast<const int16_t *>(pcm), out.decoded_size / sizeof(int16_t));
                    esp_codec_dev_write(s_spk, pcm, out.decoded_size);
                } else {
                    volume = -1;
                }
                xSemaphoreGive(s_spk_lock);
            }
        }
        free(p.data);
        s_queued--;
    }
    if (dec) {
        esp_opus_dec_close(dec);
    }
    free(pcm);
    ESP_LOGD(TAG, "play task: %u bytes of stack left", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_player_done = true;
    vTaskDeleteWithCaps(nullptr);
}

/* ---- session task ---- */

void close_codecs()
{
    if (s_mic_open) {
        esp_codec_dev_close(s_mic);
        s_mic_open = false;
    }
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    if (s_spk_open) {
        esp_codec_dev_close(s_spk);
        s_spk_open = false;
    }
    xSemaphoreGive(s_spk_lock);
    if (s_spk) {
        phone_audio_speaker_give();
        s_spk = nullptr;
    }
}

/* Speaker and microphone at 16 kHz mono; false when the speaker is busy (music) or a codec fails */
bool open_codecs()
{
    s_spk = static_cast<esp_codec_dev_handle_t>(phone_audio_speaker_take());
    if (!s_spk) {
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Loa đang được dùng (nhạc?). Hãy dừng rồi thử lại.",
                                              "The speaker is in use (music?). Stop it and try again."));
        return false;
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    xSemaphoreTake(s_spk_lock, portMAX_DELAY);
    s_spk_open = esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK;
    if (s_spk_open) {
        esp_codec_dev_set_out_vol(s_spk, phone_audio_get_volume());
    }
    xSemaphoreGive(s_spk_lock);
    if (!s_mic_open) {
        s_mic = bsp_audio_codec_microphone_init();
        s_mic_open = s_mic && esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK;
        if (s_mic_open) {
            esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
        }
    }
    if (!s_spk_open || !s_mic_open) {
        ESP_LOGE(TAG, "codec open failed: speaker %d, microphone %d", s_spk_open, s_mic_open);
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Không mở được micro hoặc loa", "Could not open the microphone or the speaker"));
        close_codecs();
        return false;
    }
    return true;
}

/* Leave the conversation: back to idle, or to `state` */
void end_talk(int state)
{
    if (s_talking) {
        s_talking = false;
        flush_audio();
        close_codecs();
    }
    s_answer_done = false;
    s_goodbye = false;
    s_mic_level = 0;
    s_state = state;
}

void listen()
{
    s_answer_done = false;
    if (esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_AUTO) == ESP_OK) {
        s_state = ASSISTANT_CHAT_LISTENING;
    } else {
        /* the server closed the conversation on its side (after a goodbye, or a long silence): a normal end */
        ESP_LOGI(TAG, "the server ended the conversation");
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        end_talk(ASSISTANT_CHAT_IDLE);
    }
}

void on_button()
{
    switch (s_state.load()) {
    case ASSISTANT_CHAT_IDLE: {
        if (!open_codecs()) {
            return;
        }
        s_talking = true;
        ref_reset();
        s_acc_n = 0;
        s_state = ASSISTANT_CHAT_OPENING;
        esp_xiaozhi_chat_audio_t audio = {};
        audio.format = "opus";
        audio.sample_rate = RATE;
        audio.channels = 1;
        audio.frame_duration = FRAME_MS;
        const esp_err_t err = esp_xiaozhi_chat_open_audio_channel(s_chat, &audio, nullptr, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "open_audio_channel: %s", esp_err_to_name(err));
            push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Không mở được kênh thoại", "Could not open the voice channel"));
            end_talk(ASSISTANT_CHAT_IDLE);
        }
        break;
    }
    case ASSISTANT_CHAT_OPENING:
    case ASSISTANT_CHAT_LISTENING:
    case ASSISTANT_CHAT_THINKING:
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        end_talk(ASSISTANT_CHAT_IDLE);
        break;
    case ASSISTANT_CHAT_SPEAKING:
        /* cut the answer and listen again */
        esp_xiaozhi_chat_send_abort_speaking(s_chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
        flush_audio();
        listen();
        break;
    default:
        break;
    }
}

/* While the answer plays: microphone and reference go to the front-end in its own chunk size; the wake word
 * heard over the answer cuts it, as a tap on the button does */
void barge_in_feed()
{
    s_mic_level = 0;
    const int chunk = assistant_afe_chunk();
    int i = 0;
    while (i < FRAME_SAMPLES) {
        const int n = std::min(chunk - s_acc_n, FRAME_SAMPLES - i);
        memcpy(s_acc_mic + s_acc_n, s_mic_buf + i, n * sizeof(int16_t));
        memcpy(s_acc_ref + s_acc_n, s_ref_buf + i, n * sizeof(int16_t));
        s_acc_n += n;
        i += n;
        if (s_acc_n == chunk) {
            assistant_afe_feed(s_acc_mic, s_acc_ref);
            s_acc_n = 0;
        }
    }
    if (assistant_afe_woke()) {
        ESP_LOGI(TAG, "wake word over the answer: barge in");
        on_button();
    }
}

/* One microphone frame: sent while listening, dropped otherwise (the clock keeps running) */
void pump_microphone()
{
    if (esp_codec_dev_read(s_mic, s_mic_buf, FRAME_SAMPLES * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    ref_take(s_ref_buf, FRAME_SAMPLES);
    if (s_state == ASSISTANT_CHAT_SPEAKING) {
        if (s_afe_on && s_barge_want) {
            barge_in_feed();
        }
        return;
    }
    if (s_state != ASSISTANT_CHAT_LISTENING || !s_enc) {
        s_mic_level = 0;
        return;
    }
    int peak = 0;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        const int v = s_mic_buf[i] < 0 ? -s_mic_buf[i] : s_mic_buf[i];
        peak = v > peak ? v : peak;
    }
    s_mic_level = peak * 100 / 32768;
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t *>(s_mic_buf);
    in.len = s_enc_in;
    esp_audio_enc_out_frame_t out = {};
    out.buffer = s_enc_buf;
    out.len = s_enc_out;
    if (esp_opus_enc_process(s_enc, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes > 0) {
        esp_xiaozhi_chat_send_audio_data(s_chat, reinterpret_cast<const char *>(s_enc_buf), out.encoded_bytes);
    }
}

bool open_encoder()
{
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RATE;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = OPUS_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 3;
    cfg.enable_fec = false;
    cfg.enable_dtx = true;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_enc) != ESP_AUDIO_ERR_OK ||
            esp_opus_enc_get_frame_size(s_enc, &s_enc_in, &s_enc_out) != ESP_AUDIO_ERR_OK) {
        s_enc = nullptr;
        return false;
    }
    s_enc_buf = static_cast<uint8_t *>(malloc(s_enc_out));
    s_mic_buf = static_cast<int16_t *>(malloc(s_enc_in > static_cast<int>(FRAME_SAMPLES * sizeof(int16_t))
                                              ? s_enc_in : FRAME_SAMPLES * sizeof(int16_t)));
    ESP_LOGI(TAG, "Opus encoder: %d bytes in, up to %d bytes out per frame", s_enc_in, s_enc_out);
    return s_enc_buf && s_mic_buf && s_enc_in == static_cast<int>(FRAME_SAMPLES * sizeof(int16_t));
}

/* ---- echo delay calibration (button of the options panel, pref "aecd") ----
 * A noise burst is played and recorded the way an answer is (60 ms writes placed on the timeline by ref_put,
 * 60 ms microphone reads advancing it by ref_take); the delay is where the recording best matches the burst.
 * The timeline does not depend on the case of the speaker: the delay is the buffers of the codecs, the air
 * between speaker and microphone adds well under 1 ms. */

constexpr int CAL_BURST = RATE * 600 / 1000;
constexpr int CAL_CHUNKS = CAL_BURST / FRAME_SAMPLES;
constexpr int CAL_REC = RATE * 1500 / 1000 / FRAME_SAMPLES * FRAME_SAMPLES;

struct CalBurst {
    const int16_t *pcm;
    int64_t start[CAL_CHUNKS];      // timeline index of each 60 ms write
    std::atomic<bool> done;
};

void cal_writer(void *arg)
{
    auto *c = static_cast<CalBurst *>(arg);
    vTaskDelay(pdMS_TO_TICKS(150));                 // let the recording get going first
    for (int k = 0; k < CAL_CHUNKS; k++) {
        xSemaphoreTake(s_spk_lock, portMAX_DELAY);
        if (s_spk_open) {
            c->start[k] = ref_put(nullptr, FRAME_SAMPLES);
            esp_codec_dev_write(s_spk, const_cast<int16_t *>(c->pcm + k * FRAME_SAMPLES), FRAME_SAMPLES * sizeof(int16_t));
        }
        xSemaphoreGive(s_spk_lock);
    }
    c->done = true;
    vTaskDelete(nullptr);
}

/* NVS writes the flash, which disables the cache: not from this task, whose stack is in PSRAM */
struct PrefJob {
    const char *key;
    int value;
    SemaphoreHandle_t done;
};

void pref_job(void *arg)
{
    auto *job = static_cast<PrefJob *>(arg);
    phone_pref_set(job->key, job->value);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

void pref_set_internal(const char *key, int value)
{
    PrefJob job = {key, value, xSemaphoreCreateBinary()};
    if (!job.done) {
        return;
    }
    if (xTaskCreate(pref_job, "assistant_pref", 3072, &job, 5, nullptr) == pdPASS) {
        xSemaphoreTake(job.done, portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "pref %s not saved", key);
    }
    vSemaphoreDelete(job.done);
}

void calibrate()
{
    auto *burst = static_cast<int16_t *>(heap_caps_malloc(CAL_BURST * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    auto *rec = static_cast<int16_t *>(heap_caps_malloc(CAL_REC * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    auto *ctx = static_cast<CalBurst *>(heap_caps_calloc(1, sizeof(CalBurst), MALLOC_CAP_SPIRAM));
    if (!burst || !rec || !ctx || !s_ref_buf) {
        heap_caps_free(burst);
        heap_caps_free(rec);
        heap_caps_free(ctx);
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Không đủ bộ nhớ để hiệu chỉnh", "Not enough memory to calibrate"));
        return;
    }
    if (phone_audio_get_volume() < 30) {
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Tăng âm lượng (từ 30%) rồi đo lại", "Raise the volume (30% or more) and retry"));
    } else if (open_codecs()) {
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Đang đo độ trễ tiếng vọng, giữ yên lặng...", "Measuring the echo delay, keep quiet..."));
        uint32_t seed = 12345;
        for (int i = 0; i < CAL_BURST; i++) {
            seed = seed * 1664525u + 1013904223u;
            burst[i] = static_cast<int16_t>((static_cast<int32_t>(seed >> 16) - 32768) / 4);    // white noise, -12 dBFS peak
        }
        ctx->pcm = burst;
        for (int k = 0; k < CAL_CHUNKS; k++) {
            ctx->start[k] = INT64_MIN;
        }
        ref_reset();
        int got = 0;
        if (xTaskCreate(cal_writer, "assistant_cal", 4096, ctx, 6, nullptr) == pdPASS) {
            while (got < CAL_REC && esp_codec_dev_read(s_mic, rec + got, FRAME_SAMPLES * sizeof(int16_t)) == ESP_CODEC_DEV_OK) {
                ref_take(s_ref_buf, FRAME_SAMPLES);
                got += FRAME_SAMPLES;
            }
            while (!ctx->done) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            ctx->done = true;
        }
        close_codecs();

        /* burst sample k * FRAME + j reaches the microphone at start[k] + j + delay */
        double e_burst = 0, e_rec = 0;
        for (int i = 0; i < CAL_BURST; i++) {
            e_burst += static_cast<double>(burst[i]) * burst[i];
        }
        for (int i = 0; i < got; i++) {
            e_rec += static_cast<double>(rec[i]) * rec[i];
        }
        constexpr int D_MIN = -400, D_MAX = 4000;
        int best = 0;
        double best_corr = 0;
        for (int d = D_MIN; d <= D_MAX; d++) {
            int64_t acc = 0;
            for (int k = 0; k < CAL_CHUNKS; k++) {
                if (ctx->start[k] == INT64_MIN) {
                    continue;
                }
                const int16_t *b = burst + k * FRAME_SAMPLES;
                for (int j = 0; j < FRAME_SAMPLES; j += 2) {
                    const int64_t i = ctx->start[k] + j + d;
                    if (i >= 0 && i < got) {
                        acc += static_cast<int32_t>(rec[i]) * b[j];
                    }
                }
            }
            const double c = std::fabs(static_cast<double>(acc));
            if (c > best_corr) {
                best_corr = c;
                best = d;
            }
        }
        const double score = (e_burst > 0 && e_rec > 0) ? best_corr * 2 / std::sqrt(e_burst * e_rec) : 0;
        ESP_LOGI(TAG, "echo delay %d samples (%d ms), score %.3f (was %d; first write at %lld, %d samples recorded)",
                 best, best * 1000 / RATE, score, s_ref_delay, static_cast<long long>(ctx->start[0]), got);
        char text[128];
        if (score >= 0.1 && best > D_MIN && best < D_MAX) {
            snprintf(text, sizeof(text), T("Độ trễ tiếng vọng: %.1f ms (trước %.1f ms), độ tin cậy %.2f",
                                           "Echo delay: %.1f ms (was %.1f ms), confidence %.2f"),
                     best * 1000.0 / RATE, s_ref_delay * 1000.0 / RATE, score);
            s_ref_delay = best;
            pref_set_internal("aecd", best);
        } else {
            snprintf(text, sizeof(text), "%s", T("Không nghe rõ tiếng loa: tăng âm lượng, giữ yên lặng rồi đo lại",
                                                 "The speaker was not heard clearly: raise the volume, keep quiet and retry"));
        }
        push_msg(ASSISTANT_CHAT_MSG_NOTICE, text);
    }
    heap_caps_free(burst);
    heap_caps_free(rec);
    heap_caps_free(ctx);
}

/* ---- wake word: only the microphone is open while the assistant is idle ---- */

bool wake_mic_open()
{
    if (s_mic_open) {
        return true;
    }
    s_mic = bsp_audio_codec_microphone_init();
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    s_mic_open = s_mic && esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK;
    if (s_mic_open) {
        esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    }
    return s_mic_open;
}

void wake_mic_close()
{
    if (s_mic_open && !s_talking) {
        esp_codec_dev_close(s_mic);
        s_mic_open = false;
    }
}

/* One chunk of the idle microphone; the wake word starts a conversation like a tap on the button */
void wake_pump()
{
    if (!wake_mic_open()) {
        ESP_LOGW(TAG, "wake word: microphone not available");
        s_wake = false;
        assistant_wake_close();
        return;
    }
    const int n = assistant_wake_chunk();
    if (esp_codec_dev_read(s_mic, s_wake_buf, n * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (assistant_wake_feed(s_wake_buf)) {
        ESP_LOGI(TAG, "wake word heard");
        on_button();
    }
}

/* ---- options (switches of the conversation page, NVS "wake" and "barge") ---- */

void options_open()
{
    const bool want_wake = s_wake_want = phone_pref_get("wake", 1);
    const bool want_barge = s_barge_want = phone_pref_get("barge", 1);
    /* both use the WakeNet model of the "model" partition */
    const bool models = (want_wake || want_barge) && assistant_wake_open();
    s_wake = models && want_wake;
    if (s_wake) {
        s_wake_buf = static_cast<int16_t *>(malloc(assistant_wake_chunk() * sizeof(int16_t)));
        s_wake = s_wake_buf != nullptr;
    }
    s_ref_delay = phone_pref_get("aecd", REF_DELAY_DEFAULT);
    if (models && want_barge) {
        /* the I2S channels first (stage 3b: created after the front-end, they found no DMA memory) */
        int16_t *ring = static_cast<int16_t *>(heap_caps_calloc(REF_RING, sizeof(int16_t), MALLOC_CAP_SPIRAM));
        taskENTER_CRITICAL(&s_ref_mux);
        s_ref_ring = ring;
        taskEXIT_CRITICAL(&s_ref_mux);
        s_afe_on = ring && open_codecs();
        close_codecs();
        s_afe_on = s_afe_on && assistant_afe_open();
        if (s_afe_on) {
            const int chunk = assistant_afe_chunk();
            s_acc_mic = static_cast<int16_t *>(heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM));
            s_acc_ref = static_cast<int16_t *>(heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM));
            s_afe_on = s_acc_mic && s_acc_ref;
        }
    }
    ESP_LOGI(TAG, "options: wake word %s, barge-in %s (echo delay %d samples)", s_wake ? "on" : "off",
             s_afe_on ? "on" : "off", s_ref_delay);
}

void options_close()
{
    wake_mic_close();
    assistant_afe_close();
    s_afe_on = false;
    heap_caps_free(s_acc_mic);
    heap_caps_free(s_acc_ref);
    s_acc_mic = s_acc_ref = nullptr;
    taskENTER_CRITICAL(&s_ref_mux);
    int16_t *ring = s_ref_ring;
    s_ref_ring = nullptr;
    taskEXIT_CRITICAL(&s_ref_mux);
    heap_caps_free(ring);
    assistant_wake_close();
    free(s_wake_buf);
    s_wake_buf = nullptr;
    s_wake = false;
}

void session_task(void *)
{
    bool ok = open_encoder();
    if (!ok) {
        ESP_LOGE(TAG, "no Opus encoder");
    }
    esp_event_handler_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, on_link_event, nullptr);
    if (ok) {
        esp_xiaozhi_chat_config_t config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
        config.audio_callback = on_audio;
        config.event_callback = on_chat_event;
        /* esp_xiaozhi prefers MQTT + UDP when it is offered both. Here the MQTT broker closes the
         * connection right after the TLS handshake, so WebSocket is used whenever the server offers it. */
        config.has_websocket_config = s_has_websocket;
        config.has_mqtt_config = s_has_mqtt && !s_has_websocket;
        ESP_LOGI(TAG, "transport: %s", config.has_mqtt_config ? "MQTT + UDP" : "WebSocket");
        /* esp_xiaozhi needs an MCP engine to start; the device tools are what the server can call */
        esp_mcp_t *mcp = nullptr;
        ok = esp_mcp_create(&mcp) == ESP_OK;
        if (ok) {
            assistant_mcp_register(mcp);
        }
        config.mcp_engine = mcp;
        config.owns_mcp_engine = true;
        if (ok && esp_xiaozhi_chat_init(&config, &s_chat) != ESP_OK) {
            esp_mcp_destroy(mcp);
            s_chat = 0;
            ok = false;
        }
        if (ok && esp_xiaozhi_chat_start(s_chat) != ESP_OK) {
            ESP_LOGE(TAG, "chat start failed");
            ok = false;
        }
    }
    s_connected = false;
    s_state = ok ? ASSISTANT_CHAT_CONNECTING : ASSISTANT_CHAT_ERROR;
    s_ref_buf = static_cast<int16_t *>(heap_caps_calloc(FRAME_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (ok) {
        options_open();
    }

    bool run = true;
    while (run) {
        int cmd;
        /* while talking the microphone read paces the loop */
        while (xQueueReceive(s_cmds, &cmd, (s_talking || (s_wake && s_wake_want && s_state == ASSISTANT_CHAT_IDLE)) ? 0 : pdMS_TO_TICKS(100)) == pdPASS) {
            switch (cmd) {
            case CMD_STOP:
                run = false;
                break;
            case CMD_OPTIONS:
                /* off: at once (the models stay until no conversation is open); on: once they are loaded */
                s_wake_want = phone_pref_get("wake", 1);
                s_barge_want = phone_pref_get("barge", 1);
                s_options_changed = true;
                ESP_LOGI(TAG, "switches: wake word %d, barge-in %d", s_wake_want, s_barge_want);
                break;
            case CMD_CALIBRATE:
                if (s_talking) {
                    push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Kết thúc hội thoại rồi đo", "End the conversation first"));
                } else {
                    s_calibrate = true;
                }
                break;
            case CMD_CONNECTED:
                s_connected = true;
                if (s_state == ASSISTANT_CHAT_CONNECTING || s_state == ASSISTANT_CHAT_ERROR) {
                    s_state = ASSISTANT_CHAT_IDLE;
                }
                break;
            case CMD_DISCONNECTED:
                s_connected = false;
                if (s_state != ASSISTANT_CHAT_OFF && s_chat) {
                    /* a link lost in the middle of a sentence is worth telling; the server hanging up
                     * after its last answer is not */
                    const int st = s_state;
                    if (s_talking && st == ASSISTANT_CHAT_SPEAKING) {
                        /* the server hung up while its last sentence is still playing: let it finish */
                        s_goodbye = true;
                        s_answer_done = true;
                        break;
                    }
                    if (s_talking && !s_goodbye && (st == ASSISTANT_CHAT_OPENING || st == ASSISTANT_CHAT_THINKING)) {
                        push_msg(ASSISTANT_CHAT_MSG_NOTICE, T("Mất kết nối với máy chủ", "Lost the connection to the server"));
                    }
                    end_talk(ASSISTANT_CHAT_CONNECTING);     /* esp_xiaozhi connects again by itself */
                }
                break;
            case CMD_BUTTON:
                if (s_chat) {
                    on_button();
                }
                break;
            case CMD_CHANNEL_OPENED:
                if (s_state == ASSISTANT_CHAT_OPENING) {
                    listen();
                }
                break;
            case CMD_GOODBYE:
                /* let the goodbye sentence play to its end */
                if (s_talking && s_state == ASSISTANT_CHAT_SPEAKING) {
                    s_goodbye = true;
                    break;
                }
                [[fallthrough]];
            case CMD_CHANNEL_CLOSED:
                if (s_talking && !(s_goodbye && s_state == ASSISTANT_CHAT_SPEAKING)) {
                    end_talk(ASSISTANT_CHAT_IDLE);
                }
                break;
            case CMD_TTS_START:
                if (s_talking) {
                    if (s_afe_on && s_state != ASSISTANT_CHAT_SPEAKING) {
                        assistant_afe_reset();
                        s_acc_n = 0;
                    }
                    s_answer_done = false;
                    s_state = ASSISTANT_CHAT_SPEAKING;
                }
                break;
            case CMD_TTS_STOP:
                if (s_state == ASSISTANT_CHAT_SPEAKING) {
                    s_answer_done = true;
                }
                break;
            default:
                break;
            }
            if (!run) {
                break;
            }
        }
        if (!run) {
            break;
        }
        if (s_calibrate && !s_talking) {
            s_calibrate = false;
            calibrate();
        }
        if (s_options_changed && !s_talking && s_chat) {
            s_options_changed = false;
            options_close();
            options_open();
        }
        if (s_talking && s_mic_open) {
            pump_microphone();
            /* the whole answer has been played: listen again */
            if (s_answer_done && s_queued <= 0 && s_state == ASSISTANT_CHAT_SPEAKING) {
                if (s_goodbye) {
                    if (s_connected) {
                        esp_xiaozhi_chat_close_audio_channel(s_chat);
                    }
                    end_talk(s_connected ? ASSISTANT_CHAT_IDLE : ASSISTANT_CHAT_CONNECTING);
                } else {
                    listen();
                }
            }
        } else if (s_wake) {
            if (s_state == ASSISTANT_CHAT_IDLE && s_wake_want) {
                wake_pump();
            } else {
                wake_mic_close();      /* not connected, or the switch is off: leave the I2S clock alone */
            }
        }
    }

    if (s_chat) {
        if (s_talking) {
            esp_xiaozhi_chat_close_audio_channel(s_chat);
        }
        end_talk(ASSISTANT_CHAT_OFF);
        esp_xiaozhi_chat_stop(s_chat);
        esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
    esp_event_handler_unregister(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID, on_link_event);
    if (s_enc) {
        esp_opus_enc_close(s_enc);
        s_enc = nullptr;
    }
    free(s_enc_buf);
    wake_mic_close();
    options_close();
    heap_caps_free(s_ref_buf);
    s_ref_buf = nullptr;
    free(s_mic_buf);
    s_enc_buf = nullptr;
    s_mic_buf = nullptr;
    ESP_LOGD(TAG, "session task: %u bytes of stack left", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_state = ASSISTANT_CHAT_OFF;
    xSemaphoreGive(s_done);
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

extern "C" void assistant_chat_start(bool has_mqtt, bool has_websocket)
{
    if (s_session) {
        return;
    }
    if (!s_cmds) {
        s_cmds = xQueueCreate(16, sizeof(int));
        s_msgs = xQueueCreate(16, sizeof(assistant_chat_msg_t *));
        s_audio = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(Packet));
        s_done = xSemaphoreCreateBinary();
        s_spk_lock = xSemaphoreCreateMutex();
    }
    xQueueReset(s_cmds);
    s_has_mqtt = has_mqtt;
    s_has_websocket = has_websocket;
    s_queued = 0;
    s_state = ASSISTANT_CHAT_CONNECTING;
    s_player_run = true;
    s_player_done = false;
    /* both stacks in PSRAM (48 KB of internal RAM otherwise); code runs from PSRAM too (XIP), and neither task
     * writes the flash */
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "assistant_play", 16384, nullptr, 6, &s_player, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_player = nullptr;
        s_player_done = true;
    }
    /* Opus encoding needs a deep stack */
    if (xTaskCreatePinnedToCoreWithCaps(session_task, "assistant_chat", 32768, nullptr, 5, &s_session, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_session = nullptr;
        s_state = ASSISTANT_CHAT_ERROR;
    }
}

extern "C" void assistant_chat_stop(void)
{
    if (s_session) {
        post(CMD_STOP);
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGE(TAG, "session task did not stop");
        }
        s_session = nullptr;
    }
    if (s_player) {
        s_player_run = false;
        for (int i = 0; i < 100 && !s_player_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_player = nullptr;
    }
    flush_audio();
    assistant_chat_msg_t *msg;
    while (s_msgs && xQueueReceive(s_msgs, &msg, 0) == pdPASS) {
        free(msg);
    }
    s_state = ASSISTANT_CHAT_OFF;
}

extern "C" void assistant_chat_options_changed(void)
{
    post(CMD_OPTIONS);
}

extern "C" void assistant_chat_calibrate(void)
{
    post(CMD_CALIBRATE);
}

extern "C" void assistant_chat_button(void)
{
    post(CMD_BUTTON);
}

extern "C" assistant_chat_state_t assistant_chat_state(void)
{
    return static_cast<assistant_chat_state_t>(s_state.load());
}

extern "C" bool assistant_chat_poll(assistant_chat_msg_t *out)
{
    assistant_chat_msg_t *msg;
    if (!s_msgs || !out || xQueueReceive(s_msgs, &msg, 0) != pdPASS) {
        return false;
    }
    *out = *msg;
    free(msg);
    return true;
}

extern "C" int assistant_chat_mic_level(void)
{
    return s_mic_level;
}
