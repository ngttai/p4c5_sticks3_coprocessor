/*
 * afe_s3: giai đoạn 4 của plan, AFE độc lập trên StickS3 (ESP32-S3).
 *
 * ES8311 xuất tham chiếu số của DAC lên kênh phải (no_dac_ref = false), nên bản ghi 2 kênh là
 * L = mic, R = ref, đúng định dạng "MR" của esp-sr. Với mỗi cấu hình AFE, chương trình:
 *   0-3 s   im lặng (đo nền)
 *   3-11 s  phát tone 1 kHz ra loa (ampli bật chỉ trong lúc này)
 *   11-14 s im lặng
 * và in theo từng giây: mức tone 1 kHz ở mic thô và ở ngõ ra AFE, ERLE = hiệu hai mức,
 * cộng CPU từng core, RAM nội tối thiểu, PSRAM trống.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "afe_s3";

#define SAMPLE_RATE   16000
#define TONE_HZ       1000
#define TONE_AMP      8000     /* -12 dBFS */
#define SPK_VOLUME    45
#define MIC_GAIN_DB   24.0f

#define PIN_SDA  47
#define PIN_SCL  48
#define PIN_MCLK 18
#define PIN_BCLK 17
#define PIN_WS   15
#define PIN_DOUT 14
#define PIN_DIN  16

#define PMIC_ADDR     0x6E
#define PMIC_GPIO_DIR 0x10
#define PMIC_GPIO_OUT 0x11
#define PMIC_GPIO_OD  0x13
#define PMIC_L3B      (1u << 2)    /* nguồn LCD + audio */
#define PMIC_SPK      (1u << 3)    /* enable ampli AW8737 */

/* esp-sr AFE V1 không có task riêng: AEC/VAD chạy ngay trong feed() và fetch() của task gọi.
 * afe_perferred_core không có tác dụng; muốn AFE nằm trên core 1 thì phải ghim hai task feed/fetch vào core 1. */
#define AFE_CORE 1

/* TEST_MODE 1 = tone 1 kHz, 2 = giọng nói tổng hợp (flite) lặp lại */
#define TEST_MODE  3
#if TEST_MODE == 3
/* double-talk: 0-1 s im, 1-2 s ba tiếng bip báo vòng mới, 3-16 s phát giọng far-end,
 * bip đơn lúc 8,0 s = BẮT ĐẦU NÓI, bip đơn lúc 15,0 s = DỪNG NÓI */
#define T_SILENCE1 3
#define T_TONE_END 17
#define T_TOTAL    20
#else
#define T_SILENCE1 3
#define T_TONE_END 15
#define T_TOTAL    17
#endif

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pmic;
static esp_codec_dev_handle_t s_spk, s_mic;

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

static void amp(bool on) { ESP_ERROR_CHECK_WITHOUT_ABORT(pmic_update(PMIC_GPIO_OUT, PMIC_SPK, on)); }

/* ---------- audio ---------- */
static esp_err_t audio_init(void)
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
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s chan");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(data_if && ctrl_if, ESP_ERR_NO_MEM, TAG, "codec if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,                 /* ampli do PMIC điều khiển, bật/tắt bằng amp() */
        .use_mclk = true,
        .no_dac_ref = false,          /* kênh phải của bản ghi = ref số từ DAC */
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

/* ---------- đo ---------- */
typedef struct {
    double raw_sq, ref_sq, out_sq;           /* tổng bình phương */
    double raw_c, raw_s, out_c, out_s;       /* Goertzel tại TONE_HZ (chiếu lên cos/sin) */
    long raw_n, out_n;
    int vad_speech, vad_n;
} win_t;

static win_t s_win[T_TOTAL + 2];
static char s_summary[4][700];
static int s_nsum;
static volatile int64_t s_t0_us;
static volatile bool s_run;
static volatile bool s_tone;
extern const int16_t speech_start[] asm("_binary_speech_raw_start");
extern const int16_t speech_end[] asm("_binary_speech_raw_end");
static const esp_afe_sr_iface_t *s_iface;
static esp_afe_sr_data_t *s_afe;
static long s_raw_idx, s_out_idx;            /* chỉ số mẫu liên tục cho pha của Goertzel */

static int cur_sec(void)
{
    int64_t d = esp_timer_get_time() - s_t0_us;
    int s = (int)(d / 1000000);
    return s < 0 ? 0 : (s > T_TOTAL ? T_TOTAL : s);
}

/* 1 kHz ở 16 kHz = đúng 16 mẫu mỗi chu kỳ: dùng bảng, tránh sin/cos kiểu double (S3 chỉ có FPU float) */
#define TBL 16
static float s_cos[TBL], s_sin[TBL];
static int16_t s_tone_tbl[TBL];
static int16_t s_beep_tbl[8];
static void tables_init(void)
{
    for (int i = 0; i < TBL; i++) {
        float ph = 2.0f * (float)M_PI * i / TBL;
        s_cos[i] = cosf(ph); s_sin[i] = sinf(ph);
        s_tone_tbl[i] = (int16_t)(TONE_AMP * sinf(ph));
    }
    for (int i = 0; i < 8; i++) s_beep_tbl[i] = (int16_t)(7000 * sinf(2.0f * (float)M_PI * i / 8));
}

static void audio_task(void *arg)
{
    const int chunk = (int)(intptr_t)arg;             /* số mẫu mỗi kênh */
    int16_t *in = heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *out = heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    long tone_idx = 0;
    while (s_run) {
        const bool tone = s_tone;
#if TEST_MODE == 3
        const int64_t el = esp_timer_get_time() - s_t0_us;
        const bool fe = el >= 3000000 && el < 16000000;
        const bool beep = (el >= 1000000 && el < 1200000) || (el >= 1400000 && el < 1600000) || (el >= 1800000 && el < 2000000) ||
                          (el >= 8000000 && el < 8300000) || (el >= 15000000 && el < 15300000);
#endif
        for (int i = 0; i < chunk; i++) {
#if TEST_MODE == 3
            static long sp = 0;
            const long sp_n = speech_end - speech_start;
            int16_t v = fe ? speech_start[sp++ % sp_n] : 0;
            if (beep) { int32_t t = v + s_beep_tbl[i & 7]; v = t > 32767 ? 32767 : (t < -32768 ? -32768 : t); }
#elif TEST_MODE == 2
            static long sp = 0;
            const long sp_n = speech_end - speech_start;
            int16_t v = tone ? speech_start[sp++ % sp_n] : 0;
#else
            int16_t v = tone ? s_tone_tbl[tone_idx & (TBL - 1)] : 0;
#endif
            tone_idx++;
            out[i * 2] = v; out[i * 2 + 1] = v;
        }
        esp_codec_dev_write(s_spk, out, chunk * 2 * sizeof(int16_t));
        if (esp_codec_dev_read(s_mic, in, chunk * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) continue;
        win_t *w = &s_win[cur_sec()];
        float sq = 0, rq = 0, cc = 0, ss = 0;
        for (int i = 0; i < chunk; i++) {
            float m = in[i * 2], r = in[i * 2 + 1];
            int k = (int)(s_raw_idx++ & (TBL - 1));
            sq += m * m; rq += r * r;
            cc += m * s_cos[k]; ss += m * s_sin[k];
        }
        w->raw_sq += sq; w->ref_sq += rq; w->raw_c += cc; w->raw_s += ss;
        w->raw_n += chunk;
        s_iface->feed(s_afe, in);
    }
    heap_caps_free(in); heap_caps_free(out);
    vTaskDelete(NULL);
}

static SemaphoreHandle_t s_fetch_done;
static void fetch_task(void *arg)
{
    while (s_run) {
        afe_fetch_result_t *r = s_iface->fetch_with_delay(s_afe, pdMS_TO_TICKS(100));
        if (!r || r->ret_value == ESP_FAIL || !r->data) continue;
        int n = r->data_size / sizeof(int16_t);
        win_t *w = &s_win[cur_sec()];
        float oq = 0, cc = 0, ss = 0;
        for (int i = 0; i < n; i++) {
            float o = r->data[i];
            int k = (int)(s_out_idx++ & (TBL - 1));
            oq += o * o; cc += o * s_cos[k]; ss += o * s_sin[k];
        }
        w->out_sq += oq; w->out_c += cc; w->out_s += ss;
        w->out_n += n;
        w->vad_n++; if (r->vad_state == VAD_SPEECH) w->vad_speech++;
    }
    xSemaphoreGive(s_fetch_done);
    vTaskDelete(NULL);
}

static double db(double x) { return 20.0 * log10(x > 1e-9 ? x : 1e-9); }
static double tone_amp(double c, double s, long n) { return n ? 2.0 * sqrt(c * c + s * s) / n : 0; }

/* CPU: phần trăm idle của từng core trong một khoảng */
typedef struct { uint32_t idle[2]; uint32_t total; } cpu_t;
static void cpu_sample(cpu_t *c)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *st = malloc(n * sizeof(TaskStatus_t));
    uint32_t total = 0;
    n = uxTaskGetSystemState(st, n, &total);
    c->total = total; c->idle[0] = c->idle[1] = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(st[i].pcTaskName, "IDLE0") == 0) c->idle[0] = st[i].ulRunTimeCounter;
        if (strcmp(st[i].pcTaskName, "IDLE1") == 0) c->idle[1] = st[i].ulRunTimeCounter;
    }
    free(st);
}

typedef struct { const char *name; afe_type_t type; afe_mode_t mode; bool aec; } cfg_t;

static void run_config(const cfg_t *c)
{
    printf("\n===== CONFIG %s =====\n", c->name);
    afe_config_t *cfg = afe_config_init("MR", NULL, c->type, c->mode);
    if (!cfg) { printf("RESULT %s afe_config_init FAILED\n", c->name); return; }
    cfg->wakenet_init = false;
    cfg->aec_init = c->aec;
    cfg->vad_init = true;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_core = 1;
    cfg = afe_config_check(cfg);
    printf("afe cfg: preferred_core=%d priority=%d\n", cfg->afe_perferred_core, cfg->afe_perferred_priority);
    s_iface = esp_afe_handle_from_config(cfg);
    s_afe = s_iface ? s_iface->create_from_config(cfg) : NULL;
    if (!s_afe) { printf("RESULT %s create FAILED\n", c->name); afe_config_free(cfg); return; }
    int chunk = s_iface->get_feed_chunksize(s_afe);
    int ch = s_iface->get_feed_channel_num(s_afe);
    s_iface->print_pipeline(s_afe);
    printf("feed chunk=%d ch=%d | internal free=%u min=%u | psram free=%u\n", chunk, ch,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (ch != 2) { printf("RESULT %s unexpected channels %d\n", c->name, ch); s_iface->destroy(s_afe); afe_config_free(cfg); return; }

    memset(s_win, 0, sizeof(s_win));
    s_raw_idx = s_out_idx = 0;
    s_tone = false; s_run = true;
    s_fetch_done = xSemaphoreCreateBinary();
    s_t0_us = esp_timer_get_time();
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, (void *)(intptr_t)chunk, 6, NULL, AFE_CORE);
    xTaskCreatePinnedToCore(fetch_task, "fetch", 6144, NULL, 5, NULL, AFE_CORE);

    cpu_t c0, c1;
    int sec = 0;
    while (sec < T_TOTAL) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int now = (int)((esp_timer_get_time() - s_t0_us) / 1000000);
#if TEST_MODE == 3
        if (now >= 1 && now < 17 && !s_tone) { amp(true); s_tone = true; }
        if (now >= 17 && s_tone) { s_tone = false; amp(false); }
#else
        if (now >= T_SILENCE1 && now < T_TONE_END && !s_tone) {
            amp(true); s_tone = true;
        }
#endif
        if (now == T_SILENCE1 + 2 && sec < now) cpu_sample(&c0);
        if (now == T_TONE_END - 1 && sec < now) cpu_sample(&c1);
#if TEST_MODE != 3
        if (now >= T_TONE_END && s_tone) { s_tone = false; amp(false); }
#endif
        sec = now;
    }
    s_run = false;
    xSemaphoreTake(s_fetch_done, pdMS_TO_TICKS(2000));
    vTaskDelay(pdMS_TO_TICKS(300));
    amp(false);

    double dt = (double)(c1.total - c0.total);              /* thời gian chạy của một core */
    printf("cpu raw: total=%u idle0=%u idle1=%u\n", (unsigned)dt, (unsigned)(c1.idle[0] - c0.idle[0]), (unsigned)(c1.idle[1] - c0.idle[1]));
    double idle0 = 100.0 * (c1.idle[0] - c0.idle[0]) / (dt > 0 ? dt : 1);
    double idle1 = 100.0 * (c1.idle[1] - c0.idle[1]) / (dt > 0 ? dt : 1);

    printf("sec |  mic_rms ref_rms out_rms | mic@1k out@1k (dBFS-ish) ERLE_dB | vad%%\n");
    double erle_sum = 0; int erle_n = 0;
    for (int s = 0; s < T_TOTAL; s++) {
        win_t *w = &s_win[s];
        double mr = w->raw_n ? sqrt(w->raw_sq / w->raw_n) : 0, rr = w->raw_n ? sqrt(w->ref_sq / w->raw_n) : 0;
        double orr = w->out_n ? sqrt(w->out_sq / w->out_n) : 0;
        double mt = db(tone_amp(w->raw_c, w->raw_s, w->raw_n)), ot = db(tone_amp(w->out_c, w->out_s, w->out_n));
        double erle = mt - ot;
        if (s >= T_SILENCE1 + 3 && s < T_TONE_END) { erle_sum += erle; erle_n++; }
        printf("%3d | %8.1f %7.1f %7.1f | %6.1f %6.1f  %7.1f | %3d%% %s\n", s, mr, rr, orr, mt, ot, erle,
               w->vad_n ? 100 * w->vad_speech / w->vad_n : 0,
               (s >= T_SILENCE1 && s < T_TONE_END) ? "tone" : "");
    }
#if TEST_MODE == 3
    {
        double fe_m = 0, fe_o = 0, dt_m = 0, dt_o = 0, fl = 0; int dt_v = 0, dt_vn = 0;
        for (int k = 6; k <= 7; k++) { fe_m += s_win[k].raw_sq / s_win[k].raw_n / 2; fe_o += s_win[k].out_sq / (s_win[k].out_n ? s_win[k].out_n : 1) / 2; }
        for (int k = 10; k <= 14; k++) { dt_m += s_win[k].raw_sq / s_win[k].raw_n / 5; dt_o += s_win[k].out_sq / (s_win[k].out_n ? s_win[k].out_n : 1) / 5; dt_v += s_win[k].vad_speech; dt_vn += s_win[k].vad_n; }
        for (int k = 17; k <= 19; k++) fl += s_win[k].out_sq / (s_win[k].out_n ? s_win[k].out_n : 1) / 3;
        double near_est = dt_m > fe_m ? dt_m - fe_m : 0;      /* năng lượng giọng người ước lượng = mic(DT) - dội âm (FE-only) */
        snprintf(s_summary[s_nsum < 4 ? s_nsum : 3], sizeof(s_summary[0]), "DOUBLETALK %s: far-end-only(s6-7) mic=%.0f out=%.0f RMS | double-talk(s10-14) mic=%.0f out=%.0f RMS vad=%d%% | near-end est=%.0f RMS | out/near=%.1f dB | silence floor out=%.0f", c->name, sqrt(fe_m), sqrt(fe_o), sqrt(dt_m), sqrt(dt_o), dt_vn ? 100 * dt_v / dt_vn : 0, sqrt(near_est), near_est > 1 ? 10 * log10((dt_o > 1 ? dt_o : 1) / near_est) : 0.0, sqrt(fl));
        s_nsum++;
        printf("DOUBLETALK %s: far-end-only(s6-7) mic=%.0f out=%.0f RMS | double-talk(s10-14) mic=%.0f out=%.0f RMS vad=%d%% | near-end est=%.0f RMS | out/near=%.1f dB | silence floor out=%.0f\n",
               c->name, sqrt(fe_m), sqrt(fe_o), sqrt(dt_m), sqrt(dt_o), dt_vn ? 100 * dt_v / dt_vn : 0, sqrt(near_est),
               near_est > 1 ? 10 * log10((dt_o > 1 ? dt_o : 1) / near_est) : 0.0, sqrt(fl));
    }
#endif
    double msq = 0, osq = 0, msq2 = 0, osq2 = 0, fl = 0;
    for (int s = 0; s < T_SILENCE1; s++) fl += s_win[s].out_n ? sqrt(s_win[s].out_sq / s_win[s].out_n) : 0;
    for (int s = T_SILENCE1; s < T_TONE_END; s++) {
        msq += s_win[s].raw_sq; osq += s_win[s].out_sq;
        if (s >= T_SILENCE1 + 4) { msq2 += s_win[s].raw_sq; osq2 += s_win[s].out_sq; }
    }
    printf("BROADBAND %s: far-end window mic/out reduction all=%.1f dB converged(s%d-%d)=%.1f dB | out floor(silence)=%.1f RMS\n", c->name,
           10 * log10(msq / (osq > 1 ? osq : 1)), T_SILENCE1 + 4, T_TONE_END - 1, 10 * log10(msq2 / (osq2 > 1 ? osq2 : 1)), fl / T_SILENCE1);
    printf("RESULT %s ERLE(avg s%d-%d)=%.1f dB | CPU busy core0=%.1f%% core1=%.1f%% | internal min free=%u KB | psram free=%u KB\n",
           c->name, T_SILENCE1 + 3, T_TONE_END - 1, erle_n ? erle_sum / erle_n : 0, 100.0 - idle0, 100.0 - idle1,
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    s_iface->destroy(s_afe); s_afe = NULL;
    afe_config_free(cfg);
    vSemaphoreDelete(s_fetch_done);
}

void app_main(void)
{
    tables_init();
    ESP_ERROR_CHECK(audio_init());
    ESP_LOGI(TAG, "audio ready, amp off");
    vTaskDelay(pdMS_TO_TICKS(1000));
#if TEST_MODE == 3
    static const cfg_t cfgs[] = {
        { "FD_HIGH", AFE_TYPE_FD, AFE_MODE_HIGH_PERF, true },
        { "VC_LOW",  AFE_TYPE_VC, AFE_MODE_LOW_COST,  true },
        { "SR_LOW_NOAEC", AFE_TYPE_SR, AFE_MODE_LOW_COST, false },
    };
#else
    static const cfg_t cfgs[] = {
        { "SR_LOW",  AFE_TYPE_SR, AFE_MODE_LOW_COST,  true },
        { "FD_LOW",  AFE_TYPE_FD, AFE_MODE_LOW_COST,  true },
        { "FD_HIGH", AFE_TYPE_FD, AFE_MODE_HIGH_PERF, true },
        { "VC_LOW",  AFE_TYPE_VC, AFE_MODE_LOW_COST,  true },
        { "SR_LOW_NOAEC", AFE_TYPE_SR, AFE_MODE_LOW_COST, false },   /* chứng: không AEC */
    };
#endif
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
#if TEST_MODE == 3
        gpio_config_t key = { .pin_bit_mask = 1ULL << 11, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
        gpio_config(&key);
        printf("WAIT KEY1 round %u/%u (%s)\n", (unsigned)i + 1, (unsigned)(sizeof(cfgs) / sizeof(cfgs[0])), cfgs[i].name);
        while (gpio_get_level(11) != 0) vTaskDelay(pdMS_TO_TICKS(20));
        while (gpio_get_level(11) == 0) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(500));
#endif
        run_config(&cfgs[i]);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("\nALL DONE\n");
    for (;;) {
        for (int k = 0; k < s_nsum && k < 4; k++) printf("%s\n", s_summary[k]);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
