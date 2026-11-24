/*
 * Audio for the KSDIY ESP32-P4C5 board, S3 variant: the microphone and the speaker are the ones of the
 * ESP32-S3 co-processor (M5StickS3, ES8311 + MEMS mic + AW8737 amp), reached over an I2S link on JP1.
 * The on-board ES8311 and ES7210 are not used.
 *
 * Link: the S3 is the I2S master, 16 kHz, 16-bit, stereo, Philips format; the P4 is the slave.
 *   JP1 GPIO46 BCLK, GPIO47 WS (inputs), GPIO48 DIN (mic), GPIO49 DOUT (playback).
 *   S3 -> P4: L = microphone after the S3's AEC/NS (AFE), R = frame counter (checks lost frames).
 *   P4 -> S3: L = playback, R = 0xA55A while the frame carries real playback (the S3 only switches its
 *             amplifier on for such frames), 0 otherwise.
 *
 * To the apps it is still esp_codec_dev: bsp_audio_codec_speaker_init() / _microphone_init() return
 * devices backed by a data interface that
 *   - speaker: takes mono 16-bit PCM at any rate (esp_codec_dev's software volume runs first), resamples
 *     it to 16 kHz and queues it for the link;
 *   - microphone: returns mono 16-bit, 16 kHz PCM from the link.
 * There is no codec interface: esp_codec_dev then uses its software volume.
 */
#include <stdatomic.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ae_rate_cvt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "bsp/esp32p4c5_bsp.h"
#include "bsp_err_check.h"

static const char *TAG = "s3link_audio";

#define LINK_RATE     16000
#define LINK_CHUNK    128                 /* frames per I2S transfer (8 ms) */
#define LINK_BCLK     GPIO_NUM_46
#define LINK_WS       GPIO_NUM_47
#define LINK_DIN      GPIO_NUM_48         /* S3 mic -> P4 */
#define LINK_DOUT     GPIO_NUM_49         /* P4 playback -> S3; S3 G3 is a strapping pin, input only there */
#define PLAYBACK_MAGIC ((int16_t)0xA55A)
#define SB_SAMPLES    4096                /* 256 ms each way */

static i2s_chan_handle_t s_tx, s_rx;
static StreamBufferHandle_t s_play_sb, s_mic_sb;
static atomic_bool s_spk_open, s_mic_open;
static atomic_uint s_ctr_err, s_play_underrun, s_mic_overrun, s_frames;
static bool s_started;

static esp_codec_dev_handle_t s_speaker, s_mic;

/* ---------- link service ---------- */
static void link_task(void *arg)
{
    int16_t *txb = heap_caps_malloc(LINK_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *rxb = heap_caps_malloc(LINK_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t mono[LINK_CHUNK];
    uint16_t expect = 0;
    bool have = false;
    size_t n;
    while (1) {
        const bool spk = atomic_load(&s_spk_open);
        size_t got = spk ? xStreamBufferReceive(s_play_sb, mono, sizeof(mono), 0) / sizeof(int16_t) : 0;
        if (got < LINK_CHUNK) {
            memset(mono + got, 0, (LINK_CHUNK - got) * sizeof(int16_t));
            if (spk && got > 0) {
                atomic_fetch_add(&s_play_underrun, 1);
            }
        }
        const int16_t tag = got > 0 ? PLAYBACK_MAGIC : 0;
        for (int i = 0; i < LINK_CHUNK; i++) {
            txb[i * 2] = mono[i];
            txb[i * 2 + 1] = tag;
        }
        i2s_channel_write(s_tx, txb, LINK_CHUNK * 2 * sizeof(int16_t), &n, 200);
        if (i2s_channel_read(s_rx, rxb, LINK_CHUNK * 2 * sizeof(int16_t), &n, 200) == ESP_OK &&
                n == LINK_CHUNK * 2 * sizeof(int16_t)) {
            for (int i = 0; i < LINK_CHUNK; i++) {
                const uint16_t r = (uint16_t)rxb[i * 2 + 1];
                if (have && r != expect) {
                    atomic_fetch_add(&s_ctr_err, 1);
                }
                expect = r + 1;
                have = true;
                mono[i] = rxb[i * 2];
            }
            if (atomic_load(&s_mic_open)) {
                /* A stream buffer takes a partial write when it is nearly full: an odd number of bytes would
                 * leave every later sample shifted by one byte (full-scale noise). Write whole chunks only. */
                if (xStreamBufferSpacesAvailable(s_mic_sb) >= sizeof(mono)) {
                    xStreamBufferSend(s_mic_sb, mono, sizeof(mono), 0);
                } else {
                    atomic_fetch_add(&s_mic_overrun, 1);
                }
            }
            atomic_fetch_add(&s_frames, 1);
            if ((atomic_load(&s_frames) % 625) == 0) {      /* every 5 s */
                ESP_LOGI(TAG, "link: frames=%u ctr_err=%u play_underrun=%u mic_overrun=%u spk=%d mic=%d play_q=%u",
                         atomic_load(&s_frames), atomic_load(&s_ctr_err), atomic_load(&s_play_underrun),
                         atomic_load(&s_mic_overrun), atomic_load(&s_spk_open), atomic_load(&s_mic_open),
                         (unsigned)xStreamBufferBytesAvailable(s_play_sb));
            }
        } else {
            have = false;
        }
    }
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    (void)i2s_config;   /* the link has a fixed format */
    if (s_started) {
        return ESP_OK;
    }
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_NUM, I2S_ROLE_SLAVE);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = LINK_CHUNK;
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx, &s_rx), TAG, "i2s_new_channel failed");
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(LINK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = LINK_BCLK, .ws = LINK_WS, .dout = LINK_DOUT, .din = LINK_DIN },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std), TAG, "rx init");
    s_play_sb = xStreamBufferCreateWithCaps(SB_SAMPLES * sizeof(int16_t), 1, MALLOC_CAP_SPIRAM);
    s_mic_sb = xStreamBufferCreateWithCaps(SB_SAMPLES * sizeof(int16_t), 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_play_sb && s_mic_sb, ESP_ERR_NO_MEM, TAG, "stream buffers");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx on");
    xTaskCreatePinnedToCoreWithCaps(link_task, "s3link", 4096, NULL, 18, NULL, 1, MALLOC_CAP_SPIRAM);
    s_started = true;
    ESP_LOGI(TAG, "S3 audio link up (I2S slave, %d Hz): BCLK %d WS %d DIN %d DOUT %d", LINK_RATE, LINK_BCLK, LINK_WS, LINK_DIN, LINK_DOUT);
    return ESP_OK;
}

void bsp_audio_link_stats(unsigned *frames, unsigned *counter_errors, unsigned *play_underruns, unsigned *mic_overruns)
{
    if (frames) *frames = atomic_load(&s_frames);
    if (counter_errors) *counter_errors = atomic_load(&s_ctr_err);
    if (play_underruns) *play_underruns = atomic_load(&s_play_underrun);
    if (mic_overruns) *mic_overruns = atomic_load(&s_mic_overrun);
}

/* ---------- data interface (esp_codec_dev) ---------- */
static uint32_t s_out_rate = LINK_RATE;
static int s_out_ch = 1;
static esp_ae_rate_cvt_handle_t s_cvt;
static uint32_t s_cvt_rate;
static int16_t *s_cvt_buf;
static uint32_t s_cvt_buf_samples;

static bool rate_supported(uint32_t r)
{
    return r % 4000 == 0 || r % 11025 == 0;
}

static void cvt_close(void)
{
    if (s_cvt) {
        esp_ae_rate_cvt_close(s_cvt);
        s_cvt = NULL;
    }
    s_cvt_rate = 0;
}

static int d_open(const audio_codec_data_if_t *h, void *cfg, int size) { return ESP_CODEC_DEV_OK; }
static bool d_is_open(const audio_codec_data_if_t *h) { return s_started; }

static int d_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, esp_codec_dev_sample_info_t *fs)
{
    if (type & ESP_CODEC_DEV_TYPE_OUT) {
        s_out_rate = fs->sample_rate ? fs->sample_rate : LINK_RATE;
        s_out_ch = fs->channel ? fs->channel : 1;
        cvt_close();
        if (s_out_rate != LINK_RATE) {
            if (!rate_supported(s_out_rate)) {
                ESP_LOGW(TAG, "%u Hz: no resampler, falling back to linear interpolation", (unsigned)s_out_rate);
            } else {
                esp_ae_rate_cvt_cfg_t c = {
                    .src_rate = s_out_rate, .dest_rate = LINK_RATE, .channel = 1, .bits_per_sample = 16,
                    .complexity = 2, .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                if (esp_ae_rate_cvt_open(&c, &s_cvt) != ESP_AE_ERR_OK) {
                    s_cvt = NULL;
                    ESP_LOGW(TAG, "resampler open failed for %u Hz", (unsigned)s_out_rate);
                }
                s_cvt_rate = s_out_rate;
            }
        }
    }
    return ESP_CODEC_DEV_OK;
}

static int d_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool en)
{
    if (!s_started) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }
    if (type & ESP_CODEC_DEV_TYPE_OUT) {
        if (en) {
            xStreamBufferReset(s_play_sb);
            atomic_store(&s_spk_open, true);
        } else {
            /* esp_codec_dev_write() returns once the PCM is queued: let the queue and the I2S DMA play out before
             * closing, or a short sound (the beep) is cut off completely */
            for (int waited = 0; waited < 1000 && xStreamBufferBytesAvailable(s_play_sb) > 0; waited += 10) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(80));
            atomic_store(&s_spk_open, false);
            xStreamBufferReset(s_play_sb);
        }
    }
    if (type & ESP_CODEC_DEV_TYPE_IN) {
        if (en) {
            xStreamBufferReset(s_mic_sb);
        }
        atomic_store(&s_mic_open, en);
    }
    return ESP_CODEC_DEV_OK;
}

/* Push len bytes of 16 kHz mono PCM into the playback queue, waiting while it is full (paces the caller) */
static int push_play(const int16_t *pcm, size_t samples)
{
    size_t bytes = samples * sizeof(int16_t), done = 0;
    int idle_ms = 0;
    while (done < bytes) {
        if (!atomic_load(&s_spk_open)) {
            return ESP_CODEC_DEV_WRONG_STATE;
        }
        size_t w = xStreamBufferSend(s_play_sb, (const uint8_t *)pcm + done, bytes - done, pdMS_TO_TICKS(50));
        if (w == 0) {
            idle_ms += 50;
            if (idle_ms > 2000) {
                return ESP_CODEC_DEV_WRITE_FAIL;
            }
        } else {
            idle_ms = 0;
            done += w;
        }
    }
    return ESP_CODEC_DEV_OK;
}

static int d_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    const int16_t *in = (const int16_t *)data;
    uint32_t n = size / (2 * s_out_ch);                    /* frames */
    if (n == 0) {
        return ESP_CODEC_DEV_OK;
    }
    if (s_out_ch == 2) {                                   /* mix down in place */
        int16_t *m = (int16_t *)data;
        for (uint32_t i = 0; i < n; i++) {
            m[i] = (int16_t)(((int)in[i * 2] + in[i * 2 + 1]) / 2);
        }
        in = m;
    }
    if (s_out_rate == LINK_RATE) {
        return push_play(in, n);
    }
    uint32_t cap = (uint32_t)((uint64_t)n * LINK_RATE / s_out_rate) + 64;
    if (s_cvt && s_cvt_rate == s_out_rate) {
        /* the converter wants an output buffer of at least what it says it may produce for this input */
        uint32_t max_out = 0;
        if (esp_ae_rate_cvt_get_max_out_sample_num(s_cvt, n, &max_out) == ESP_AE_ERR_OK && max_out > cap) {
            cap = max_out;
        }
    }
    if (cap > s_cvt_buf_samples) {
        heap_caps_free(s_cvt_buf);
        s_cvt_buf = heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        s_cvt_buf_samples = s_cvt_buf ? cap : 0;
        if (!s_cvt_buf) {
            return ESP_CODEC_DEV_NO_MEM;
        }
    }
    uint32_t out_n = cap;
    if (s_cvt && s_cvt_rate == s_out_rate) {
        if (esp_ae_rate_cvt_process(s_cvt, (esp_ae_sample_t)in, n, (esp_ae_sample_t)s_cvt_buf, &out_n) != ESP_AE_ERR_OK) {
            return ESP_CODEC_DEV_WRITE_FAIL;
        }
    } else {                                               /* linear interpolation, rate not supported by the converter */
        out_n = (uint32_t)((uint64_t)n * LINK_RATE / s_out_rate);
        if (out_n > cap) out_n = cap;
        for (uint32_t i = 0; i < out_n; i++) {
            const uint64_t pos = (uint64_t)i * s_out_rate * 256 / LINK_RATE;
            const uint32_t k = (uint32_t)(pos >> 8), f = (uint32_t)(pos & 255);
            const int a = in[k < n ? k : n - 1], b = in[k + 1 < n ? k + 1 : n - 1];
            s_cvt_buf[i] = (int16_t)((a * (256 - f) + b * f) >> 8);
        }
    }
    return push_play(s_cvt_buf, out_n);
}

static int d_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    int done = 0, idle_ms = 0;
    while (done < size) {
        if (!atomic_load(&s_mic_open)) {
            return ESP_CODEC_DEV_WRONG_STATE;
        }
        size_t r = xStreamBufferReceive(s_mic_sb, data + done, size - done, pdMS_TO_TICKS(50));
        if (r == 0) {
            idle_ms += 50;
            if (idle_ms > 1000) {
                return ESP_CODEC_DEV_READ_FAIL;
            }
        } else {
            idle_ms = 0;
            done += r;
        }
    }
    return ESP_CODEC_DEV_OK;
}

static int d_close(const audio_codec_data_if_t *h) { return ESP_CODEC_DEV_OK; }

static const audio_codec_data_if_t s_data_if = {
    .open = d_open,
    .is_open = d_is_open,
    .enable = d_enable,
    .set_fmt = d_set_fmt,
    .read = d_read,
    .write = d_write,
    .close = d_close,
};

/* ---------- BSP API ---------- */
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (s_speaker != NULL) {
        return s_speaker;
    }
    if (bsp_audio_init(NULL) != ESP_OK) {
        return NULL;
    }
    esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = NULL, .data_if = &s_data_if };
    s_speaker = esp_codec_dev_new(&cfg);
    if (s_speaker == NULL) {
        ESP_LOGE(TAG, "speaker device create failed");
        return NULL;
    }
    /* The StickS3 speaker is small: the default -50..0 dB curve leaves it inaudible below ~50%.
     * -24 dB at 1% .. -8 dB at 100% (the S3 side is at 0 dB; 0 is still mute) */
    esp_codec_dev_vol_map_t map[2] = { { .vol = 0, .db_value = -24.0f }, { .vol = 100, .db_value = -8.0f } };
    esp_codec_dev_vol_curve_t curve = { .vol_map = map, .count = 2 };
    esp_codec_dev_set_vol_curve(s_speaker, &curve);
    return s_speaker;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (s_mic != NULL) {
        return s_mic;
    }
    if (bsp_audio_init(NULL) != ESP_OK) {
        return NULL;
    }
    esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = NULL, .data_if = &s_data_if };
    s_mic = esp_codec_dev_new(&cfg);
    if (s_mic == NULL) {
        ESP_LOGE(TAG, "mic device create failed");
    }
    return s_mic;
}
