/*
 * audio_fe for the StickS3 (co-processor of the P4C5), phase 5 of the plan.
 *
 *   ES8311 (I2S0, 16 kHz, L = mic, R = digital DAC reference)
 *      -> codec_task (core 1) -> AFE (FD, AEC/NS/VAD, runs inside feed/fetch) -> fetch_task (core 1)
 *      -> txq -> link_task (core 0) -> I2S1 master -> P4 (L = clean mic, R = frame counter)
 *   P4 -> I2S1 (L = playback) -> link_task -> rxq -> codec_task -> ES8311 -> loa
 *
 * The two clock domains (I2S0 and I2S1) are both mastered by the S3 but are independent, so a stream buffer sits between them;
 * the underrun/overrun counters show whether they drift apart.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio_fe.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

static const char *TAG = "audio_fe";
#ifdef CONFIG_AUDIO_FE_BYPASS
#define AFE_MODE_NAME "BYPASS"
#else
#define AFE_MODE_NAME "AFE"
#endif

#define SAMPLE_RATE   16000
#define SPK_VOLUME    100
#define MIC_GAIN_DB   24.0f
#define LINK_CHUNK    128          /* I2S1 frames per transfer (8 ms) */
#define LINK_DMA_DESC 4            /* 4 x 8 ms = 32 ms of DMA buffer */
#define QUEUE_SAMPLES 8192         /* ~0,5 s */
#define PLAYBACK_MAGIC 0xA55A      /* R channel of playback frames from the P4 */

/* codec ES8311 (I2S0) */
#define PIN_SDA  47
#define PIN_SCL  48
#define PIN_MCLK 18
#define PIN_BCLK 17
#define PIN_WS   15
#define PIN_DOUT 14
#define PIN_DIN  16
/* link to the P4 (I2S1, the S3 is master) */
#define LINK_BCLK 43
#define LINK_WS   44
#define LINK_DOUT 4     /* clean mic -> P4 GPIO48 */
#define LINK_DIN  3     /* playback from P4 GPIO49; G3 is a strapping pin, input only */

#define PMIC_ADDR     0x6E
#define PMIC_GPIO_DIR 0x10
#define PMIC_GPIO_OUT 0x11
#define PMIC_GPIO_OD  0x13
#define PMIC_L3B      (1u << 2)
#define PMIC_SPK      (1u << 3)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pmic;
static esp_codec_dev_handle_t s_spk, s_mic;
static i2s_chan_handle_t s_link_tx, s_link_rx;
static StreamBufferHandle_t s_txq, s_rxq;   /* mono int16 */
static bool s_amp_on;

static const esp_afe_sr_iface_t *s_iface;
static esp_afe_sr_data_t *s_afe;
static int s_chunk;

static int64_t s_feed_ts[256];
static volatile unsigned s_feed_n;
static unsigned long long s_out_samples;
static atomic_ullong s_lag_sum_us;
static atomic_uint s_lag_cnt, s_lag_max_us;

static struct {
    atomic_uint codec_chunks, fetch_chunks, link_frames;
    atomic_uint tx_underrun, tx_drop, rx_overrun, rx_underrun, vad_speech, vad_total;
    atomic_uint rx_peak;
} st;

/* ---------- PMIC ---------- */
static esp_err_t pmic_update(uint8_t reg, uint8_t mask, bool on)
{
    uint8_t v = 0;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) err = i2c_master_transmit_receive(s_pmic, &reg, 1, &v, 1, 50);
    ESP_RETURN_ON_ERROR(err, TAG, "pmic read");
    uint8_t want = on ? (v | mask) : (v & ~mask);
    if (want == v) return ESP_OK;
    const uint8_t buf[2] = { reg, want };
    err = ESP_FAIL;
    for (int i = 0; i < 3 && err != ESP_OK; i++) err = i2c_master_transmit(s_pmic, buf, 2, 50);
    return err;
}

static void amp(bool on)
{
    if (on == s_amp_on) return;
    if (pmic_update(PMIC_GPIO_OUT, PMIC_SPK, on) == ESP_OK) s_amp_on = on;
}

/* ---------- ES8311 ---------- */
static esp_err_t codec_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c");
    const i2c_device_config_t pmic_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = PMIC_ADDR, .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &pmic_cfg, &s_pmic), TAG, "pmic");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OD, PMIC_L3B | PMIC_SPK, false), TAG, "push-pull");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_DIR, PMIC_L3B | PMIC_SPK, true), TAG, "dir");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_SPK, false), TAG, "amp off");
    ESP_RETURN_ON_ERROR(pmic_update(PMIC_GPIO_OUT, PMIC_L3B, true), TAG, "audio power");
    vTaskDelay(pdMS_TO_TICKS(30));

    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s0 chan");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s0 tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s0 rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s0 tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s0 rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_ERR_NO_MEM, TAG, "codec if");
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .use_mclk = true,
        .no_dac_ref = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec, ESP_FAIL, TAG, "ES8311 not responding");
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec, .data_if = data_if };
    s_spk = esp_codec_dev_new(&out_cfg);
    s_mic = esp_codec_dev_new(&in_cfg);
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "codec dev");
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .sample_rate = SAMPLE_RATE };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open spk");
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open mic");
    esp_codec_dev_set_out_vol(s_spk, SPK_VOLUME);
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    return ESP_OK;
}

/* ---------- link I2S1 ---------- */
static esp_err_t link_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_desc_num = LINK_DMA_DESC;
    cc.dma_frame_num = LINK_CHUNK;
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_link_tx, &s_link_rx), TAG, "i2s1 chan");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = LINK_BCLK, .ws = LINK_WS, .dout = LINK_DOUT, .din = LINK_DIN },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_link_tx, &std_cfg), TAG, "i2s1 tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_link_rx, &std_cfg), TAG, "i2s1 rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_link_tx), TAG, "i2s1 tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_link_rx), TAG, "i2s1 rx on");
    return ESP_OK;
}

/* link_task (core 0): writes frames with L = clean mic, R = 16-bit counter; reads playback from the P4 */
static void link_task(void *arg)
{
    int16_t *txb = heap_caps_malloc(LINK_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *rxb = heap_caps_malloc(LINK_CHUNK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t mono[LINK_CHUNK];
    int16_t echo_buf[LINK_CHUNK] = { 0 };
    (void)echo_buf;
    uint16_t counter = 0;
    size_t n;
    while (1) {
#ifdef CONFIG_AUDIO_FE_LINK_ECHO
        memcpy(mono, echo_buf, sizeof(mono));
        size_t got = LINK_CHUNK;
#else
        size_t got = xStreamBufferReceive(s_txq, mono, sizeof(mono), 0) / sizeof(int16_t);
#endif
        if (got < LINK_CHUNK) {
            memset(mono + got, 0, (LINK_CHUNK - got) * sizeof(int16_t));
            atomic_fetch_add(&st.tx_underrun, LINK_CHUNK - got);
        }
        for (int i = 0; i < LINK_CHUNK; i++) { txb[i * 2] = mono[i]; txb[i * 2 + 1] = (int16_t)counter++; }
        i2s_channel_write(s_link_tx, txb, LINK_CHUNK * 2 * sizeof(int16_t), &n, 200);
        if (i2s_channel_read(s_link_rx, rxb, LINK_CHUNK * 2 * sizeof(int16_t), &n, 200) == ESP_OK && n == LINK_CHUNK * 2 * sizeof(int16_t)) {
            /* The P4 puts the magic 0xA55A in the R channel of every valid playback frame; with G3 floating or no P4 it reads garbage,
             * and garbage has no magic, so it is treated as silence (amplifier stays off, no noise played) */
            int magic = 0;
            for (int i = 0; i < LINK_CHUNK; i++) magic += (rxb[i * 2 + 1] == (int16_t)PLAYBACK_MAGIC);
            const bool valid = magic > LINK_CHUNK * 3 / 4;
            int peak = 0;
            for (int i = 0; i < LINK_CHUNK; i++) {
                mono[i] = valid ? rxb[i * 2] : 0;
                int a = mono[i] < 0 ? -mono[i] : mono[i];
                if (a > peak) peak = a;
            }
#ifdef CONFIG_AUDIO_FE_LINK_ECHO
            memcpy(echo_buf, mono, sizeof(mono));
#else
            if (xStreamBufferSend(s_rxq, mono, sizeof(mono), 0) < sizeof(mono)) atomic_fetch_add(&st.rx_overrun, LINK_CHUNK);
#endif
            if ((unsigned)peak > atomic_load(&st.rx_peak)) atomic_store(&st.rx_peak, peak);
        }
        atomic_fetch_add(&st.link_frames, 1);
    }
}

/* ---------- AFE ---------- */
#if !CONFIG_AUDIO_FE_BYPASS
static esp_err_t afe_setup(void)
{
    afe_config_t *cfg = afe_config_init("MR", NULL, AFE_TYPE_FD,
#if CONFIG_AUDIO_FE_FD_HIGH
                                        AFE_MODE_HIGH_PERF);
#else
                                        AFE_MODE_LOW_COST);
#endif
    ESP_RETURN_ON_FALSE(cfg, ESP_FAIL, TAG, "afe_config_init");
    cfg->wakenet_init = false;
    cfg->aec_init = true;
    cfg->vad_init = true;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg = afe_config_check(cfg);
    s_iface = esp_afe_handle_from_config(cfg);
    s_afe = s_iface ? s_iface->create_from_config(cfg) : NULL;
    ESP_RETURN_ON_FALSE(s_afe, ESP_FAIL, TAG, "AFE create");
    s_chunk = s_iface->get_feed_chunksize(s_afe);
    ESP_RETURN_ON_FALSE(s_iface->get_feed_channel_num(s_afe) == 2, ESP_FAIL, TAG, "AFE channels");
    s_iface->print_pipeline(s_afe);
    return ESP_OK;
}

static void fetch_task(void *arg)
{
    while (1) {
        afe_fetch_result_t *r = s_iface->fetch_with_delay(s_afe, pdMS_TO_TICKS(100));
        if (!r || r->ret_value == ESP_FAIL || !r->data) continue;
        if (xStreamBufferSend(s_txq, r->data, r->data_size, 0) < (size_t)r->data_size) atomic_fetch_add(&st.tx_drop, r->data_size / 2);
        atomic_fetch_add(&st.fetch_chunks, 1);
        /* AFE lag: from when the chunk holding the output samples has been read from the codec to when fetch returns those samples */
        s_out_samples += r->data_size / 2;
        if (s_chunk > 0) {
            unsigned q = (unsigned)((s_out_samples - 1) / s_chunk);
            if (q < s_feed_n && s_feed_n - q < 256) {
                int64_t lag = esp_timer_get_time() - s_feed_ts[q & 255];
                atomic_fetch_add(&s_lag_sum_us, (unsigned long long)lag);
                atomic_fetch_add(&s_lag_cnt, 1);
                if ((unsigned)lag > atomic_load(&s_lag_max_us)) atomic_store(&s_lag_max_us, (unsigned)lag);
            }
        }
        atomic_fetch_add(&st.vad_total, 1);
        if (r->vad_state == VAD_SPEECH) atomic_fetch_add(&st.vad_speech, 1);
    }
}
#endif

/* codec_task (core 1): playback -> speaker, reads mic + ref, feeds the AFE (or sends the L mic straight to the link in bypass) */
static void codec_task(void *arg)
{
    const int chunk = s_chunk > 0 ? s_chunk : 512;
    int16_t *in = heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *out = heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *mono = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int64_t last_loud = 0;
    while (1) {
        size_t got = xStreamBufferReceive(s_rxq, mono, chunk * sizeof(int16_t), 0) / sizeof(int16_t);
        if (got < (size_t)chunk) {
            if (got) memmove(mono + (chunk - got), mono, got * sizeof(int16_t));   /* keep the tail, pad zeros at the front */
            memset(mono, 0, (chunk - got) * sizeof(int16_t));
            if (got) atomic_fetch_add(&st.rx_underrun, chunk - got); else if (s_amp_on) atomic_fetch_add(&st.rx_underrun, chunk);
        }
        int peak = 0;
        for (int i = 0; i < chunk; i++) {
            out[i * 2] = out[i * 2 + 1] = mono[i];
            int a = mono[i] < 0 ? -mono[i] : mono[i];
            if (a > peak) peak = a;
        }
        int64_t now = esp_timer_get_time();
#ifdef CONFIG_AUDIO_FE_AMP_ALWAYS_ON
        amp(true);
#else
        if (peak > 30) { last_loud = now; amp(true); }
        else if (s_amp_on && now - last_loud > 2000000) amp(false);
#endif
        esp_codec_dev_write(s_spk, out, chunk * 2 * sizeof(int16_t));
        if (esp_codec_dev_read(s_mic, in, chunk * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) continue;
        atomic_fetch_add(&st.codec_chunks, 1);
#if CONFIG_AUDIO_FE_BYPASS
        for (int i = 0; i < chunk; i++) mono[i] = in[i * 2];
        if (xStreamBufferSend(s_txq, mono, chunk * sizeof(int16_t), 0) < chunk * sizeof(int16_t)) atomic_fetch_add(&st.tx_drop, chunk);
#else
        s_feed_ts[s_feed_n & 255] = esp_timer_get_time();
        s_feed_n++;
        s_iface->feed(s_afe, in);
#endif
    }
}

typedef struct { uint32_t idle[2]; uint32_t total; } cpu_t;
static void cpu_sample(cpu_t *c)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = heap_caps_malloc(n * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    if (!ts) return;
    uint32_t total = 0;
    n = uxTaskGetSystemState(ts, n, &total);
    c->total = total; c->idle[0] = c->idle[1] = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(ts[i].pcTaskName, "IDLE0") == 0) c->idle[0] = ts[i].ulRunTimeCounter;
        if (strcmp(ts[i].pcTaskName, "IDLE1") == 0) c->idle[1] = ts[i].ulRunTimeCounter;
    }
    heap_caps_free(ts);
}

static void stats_task(void *arg)
{
    cpu_t c0, c1;
    cpu_sample(&c0);
    unsigned prev[4] = { 0 };
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        unsigned cc = atomic_load(&st.codec_chunks), fc = atomic_load(&st.fetch_chunks), lf = atomic_load(&st.link_frames);
        unsigned vs = atomic_exchange(&st.vad_speech, 0), vt = atomic_exchange(&st.vad_total, 0);
        ESP_LOGI(TAG, "5s: codec=%u fetch=%u link=%u | tx_underrun=%u tx_drop=%u rx_overrun=%u rx_underrun=%u | rx_peak=%u amp=%d vad=%u%% | int free=%u min=%u",
                 cc - prev[0], fc - prev[1], lf - prev[2], atomic_load(&st.tx_underrun), atomic_load(&st.tx_drop),
                 atomic_load(&st.rx_overrun), atomic_load(&st.rx_underrun), atomic_exchange(&st.rx_peak, 0), s_amp_on,
                 vt ? 100 * vs / vt : 0, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        cpu_sample(&c1);
        double dt = (double)(c1.total - c0.total);
        double b0 = dt > 0 ? 100.0 - 100.0 * (c1.idle[0] - c0.idle[0]) / dt : 0, b1 = dt > 0 ? 100.0 - 100.0 * (c1.idle[1] - c0.idle[1]) / dt : 0;
        c0 = c1;
        unsigned lc = atomic_exchange(&s_lag_cnt, 0); unsigned long long ls = atomic_exchange(&s_lag_sum_us, 0); unsigned lm = atomic_exchange(&s_lag_max_us, 0);
        ESP_LOGI(TAG, "cpu: core0=%.1f%% core1=%.1f%% | afe lag (chunk done -> fetch): avg=%.1f ms max=%.1f ms (n=%u)", b0, b1,
                 lc ? ls / 1000.0 / lc : 0.0, lm / 1000.0, lc);
        prev[0] = cc; prev[1] = fc; prev[2] = lf;
    }
}

esp_err_t audio_fe_start(void)
{
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "codec");
    /* 2 x 16 KB: kept in PSRAM, leaving internal RAM for WiFi/hosted/AFE */
    s_txq = xStreamBufferCreateWithCaps(QUEUE_SAMPLES * sizeof(int16_t), 1, MALLOC_CAP_SPIRAM);
    s_rxq = xStreamBufferCreateWithCaps(QUEUE_SAMPLES * sizeof(int16_t), 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_txq && s_rxq, ESP_ERR_NO_MEM, TAG, "stream buffers");
#if !CONFIG_AUDIO_FE_BYPASS
    ESP_RETURN_ON_ERROR(afe_setup(), TAG, "afe");
#endif
    ESP_RETURN_ON_ERROR(link_init(), TAG, "link");
    xTaskCreatePinnedToCore(link_task, "link", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCoreWithCaps(codec_task, "codec", 6144, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM);
#if !CONFIG_AUDIO_FE_BYPASS
    xTaskCreatePinnedToCoreWithCaps(fetch_task, "afe_fetch", 6144, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
#endif
    xTaskCreatePinnedToCoreWithCaps(stats_task, "fe_stats", 3072, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "audio_fe started (%s), AFE chunk=%d", AFE_MODE_NAME, s_chunk);
    return ESP_OK;
}
