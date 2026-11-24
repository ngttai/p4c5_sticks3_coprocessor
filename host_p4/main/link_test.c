/*
 * I2S link test, P4 (slave) <-> StickS3 (master), phase 5.
 *   GPIO46 BCLK, GPIO47 WS (inputs), GPIO48 DIN (clean mic from the S3), GPIO49 DOUT (playback to the S3).
 * S3 -> P4 frame: L = mic (clean when the AFE is on), R = 16-bit counter, +1 per frame (counts bad/lost frames).
 * P4 -> S3 frame: L = playback, R = magic 0xA55A (valid frame).
 *
 * Commands (run in the background so iperf can run at the same time):
 *   linktest [seconds] [tone]   count frames, tone = send a 1 kHz tone to the S3
 *   linklat  [count]            measure latency with a click pulse: P4 -> S3 -> speaker -> mic -> P4 (S3 firmware in AFE bypass)
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LINK_BCLK 46
#define LINK_WS   47
#define LINK_DIN  48
#define LINK_DOUT 49
#define SAMPLE_RATE 16000
#define CHUNK 128
#define MAGIC ((int16_t)0xA55A)

static bool link_open(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = CHUNK;
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, tx, rx) != ESP_OK) { printf("i2s_new_channel failed\n"); return false; }
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = LINK_BCLK, .ws = LINK_WS, .dout = LINK_DOUT, .din = LINK_DIN },
    };
    if (i2s_channel_init_std_mode(*tx, &std) != ESP_OK || i2s_channel_init_std_mode(*rx, &std) != ESP_OK) {
        printf("i2s init failed\n"); return false;
    }
    i2s_channel_enable(*tx); i2s_channel_enable(*rx);
    return true;
}

static void link_close(i2s_chan_handle_t tx, i2s_chan_handle_t rx)
{
    i2s_channel_disable(tx); i2s_channel_disable(rx);
    i2s_del_channel(tx); i2s_del_channel(rx);
}

static void linktest_run(int secs, bool tone)
{
    i2s_chan_handle_t tx, rx;
    if (!link_open(&tx, &rx)) return;
    static int16_t rxb[CHUNK * 2], txb[CHUNK * 2];
    long tone_idx = 0;
    uint16_t expect = 0; bool have = false;
    unsigned total_frames = 0, total_err = 0, total_to = 0, total_zero_l = 0;
    int64_t t0 = esp_timer_get_time(), tsec = t0;
    double sq = 0; int peak = 0; unsigned nsec = 0, errsec = 0;
    printf("linktest: %d s, tone=%d (the S3 must be running audio_fe)\n", secs, tone);
    while (esp_timer_get_time() - t0 < (int64_t)secs * 1000000) {
        for (int i = 0; i < CHUNK; i++) {
            int16_t v = tone ? (int16_t)(8000.0f * sinf(2.0f * (float)M_PI * (tone_idx % 16) / 16.0f)) : 0;
            tone_idx++;
            txb[i * 2] = v; txb[i * 2 + 1] = MAGIC;
        }
        size_t n = 0;
        i2s_channel_write(tx, txb, sizeof(txb), &n, 300);
        if (i2s_channel_read(rx, rxb, sizeof(rxb), &n, 300) != ESP_OK || n != sizeof(rxb)) { total_to++; have = false; continue; }
        for (int i = 0; i < CHUNK; i++) {
            uint16_t r = (uint16_t)rxb[i * 2 + 1];
            if (have && r != expect) { total_err++; errsec++; }
            expect = r + 1; have = true;
            int a = rxb[i * 2] < 0 ? -rxb[i * 2] : rxb[i * 2];
            if (a > peak) peak = a;
            sq += (double)rxb[i * 2] * rxb[i * 2];
            if (rxb[i * 2] == 0) total_zero_l++;
        }
        nsec += CHUNK; total_frames += CHUNK;
        if (esp_timer_get_time() - tsec >= 1000000) {
            printf("  t=%2d s: frames=%u  L rms=%.0f peak=%d  counter errors=%u\n", (int)((esp_timer_get_time() - t0) / 1000000),
                   nsec, sqrt(sq / (nsec ? nsec : 1)), peak, errsec);
            sq = 0; peak = 0; nsec = 0; errsec = 0; tsec = esp_timer_get_time();
        }
    }
    printf("LINKTEST RESULT: frames=%u (%.1f s) counter_errors=%u read_timeouts=%u zero_L_samples=%.1f%%\n", total_frames,
           total_frames / (float)SAMPLE_RATE, total_err, total_to, total_frames ? 100.0 * total_zero_l / total_frames : 0.0);
    link_close(tx, rx);
}

static int cmp_i(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* Click: the P4 writes one large-amplitude 1 kHz chunk and times until the mic (through the speaker) returns a sample above the threshold. */
static void linklat_run(int trials)
{
    i2s_chan_handle_t tx, rx;
    if (!link_open(&tx, &rx)) return;
    static int16_t rxb[CHUNK * 2], txb[CHUNK * 2];
    int lat[32]; int nl = 0;
    if (trials > 32) trials = 32;
    printf("linklat: %d clicks (1 kHz, 8 ms, amplitude 24000), the S3 should be in AFE bypass mode\n", trials);
    /* 1 second of silence so the amplifier switches off and the AFE settles */
    for (int t = 0; t < trials; t++) {
        int64_t tstart = esp_timer_get_time();
        int64_t t_click = 0; bool sent = false, found = false;
        int base_peak = 0;
        while (esp_timer_get_time() - tstart < 1800000) {
            bool click_now = !sent && esp_timer_get_time() - tstart > 900000;
            for (int i = 0; i < CHUNK; i++) {
                int16_t v = click_now ? (int16_t)(24000.0f * sinf(2.0f * (float)M_PI * (i % 16) / 16.0f)) : 0;
                txb[i * 2] = v; txb[i * 2 + 1] = MAGIC;
            }
            size_t n = 0;
            if (click_now) { t_click = esp_timer_get_time(); sent = true; }
            i2s_channel_write(tx, txb, sizeof(txb), &n, 300);
            if (i2s_channel_read(rx, rxb, sizeof(rxb), &n, 300) != ESP_OK || n != sizeof(rxb)) continue;
            int64_t t_rx = esp_timer_get_time();
            int pk = 0;
            for (int i = 0; i < CHUNK; i++) { int a = rxb[i * 2] < 0 ? -rxb[i * 2] : rxb[i * 2]; if (a > pk) pk = a; }
            if (!sent) { if (esp_timer_get_time() - tstart > 300000 && pk > base_peak) base_peak = pk; }
            else if (!found && pk > (base_peak * 3 > 12000 ? base_peak * 3 : 12000)) {
                found = true;
                if (nl < 32) lat[nl++] = (int)((t_rx - t_click) / 1000);
                printf("  click %2d: %d ms (floor peak %d, detected peak %d)\n", t + 1, (int)((t_rx - t_click) / 1000), base_peak, pk);
            }
        }
        if (!found) printf("  click %2d: not detected (floor peak %d)\n", t + 1, base_peak);
    }
    if (nl) {
        qsort(lat, nl, sizeof(int), cmp_i);
        printf("LINKLAT RESULT: n=%d min=%d median=%d max=%d ms (electrical, round trip over the link, including DMA both ways; one way is about half)\n", nl, lat[0], lat[nl / 2], lat[nl - 1]);
    } else {
        printf("LINKLAT RESULT: no click detected\n");
    }
    link_close(tx, rx);
}

typedef struct { int kind; int arg; } job_t;
static void job_task(void *p)
{
    job_t j = *(job_t *)p; free(p);
    if (j.kind == 0) linktest_run(j.arg >> 1, j.arg & 1); else linklat_run(j.arg);
    vTaskDelete(NULL);
}

static int start_job(int kind, int arg)
{
    job_t *j = malloc(sizeof(job_t)); j->kind = kind; j->arg = arg;
    xTaskCreate(job_task, "linkjob", 4096, j, 5, NULL);
    return 0;
}

static int linktest_cmd(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    bool tone = argc > 2 && strcmp(argv[2], "tone") == 0;
    if (secs < 1) secs = 1;
    return start_job(0, (secs << 1) | (tone ? 1 : 0));
}

static int linklat_cmd(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 8;
    return start_job(1, n < 1 ? 1 : n);
}

void register_link_test(void)
{
    const esp_console_cmd_t c1 = { .command = "linktest", .help = "I2S link to the StickS3: linktest [seconds] [tone]", .hint = NULL, .func = &linktest_cmd };
    const esp_console_cmd_t c2 = { .command = "linklat", .help = "Measure link latency with a click pulse: linklat [count]", .hint = NULL, .func = &linklat_cmd };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c1));
    ESP_ERROR_CHECK(esp_console_cmd_register(&c2));
}
