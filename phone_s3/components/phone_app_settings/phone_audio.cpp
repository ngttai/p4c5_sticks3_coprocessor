#include "phone_audio.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

#define TAG "AUD"

namespace {

constexpr int RATE = 16000;
constexpr int TONE_HZ = 1000;
constexpr int TONE_MS = 150;
constexpr int SAMPLES = RATE * TONE_MS / 1000;
/* Silence around the tone: the amplifier needs a moment after the codec opens, and closing the codec
 * stops the I2S DMA with what is still queued (6 x 240 frames, ~90 ms at 16 kHz) */
constexpr int LEAD_MS = 40;
constexpr int TAIL_MS = 120;

std::atomic<int> s_volume{60};
std::atomic<bool> s_busy{false};
esp_codec_dev_handle_t s_spk;   // only touched by whoever holds s_busy: the beep task or the music player

void beep_task(void *)
{
    constexpr int SILENCE = RATE * TAIL_MS / 1000;
    /* In PSRAM and only while beeping (8.6 KB); the codec copies them into its DMA buffers */
    auto *tone = static_cast<int16_t *>(heap_caps_malloc(SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    auto *silence = static_cast<int16_t *>(heap_caps_calloc(SILENCE, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = RATE;
    fs.channel = 1;
    fs.bits_per_sample = 16;
    /* The codec is opened only for the tone, so the speaker amplifier stays off otherwise */
    if (tone && silence && s_spk && esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_out_vol(s_spk, s_volume.load());
        for (int i = 0; i < SAMPLES; i++) {
            /* 10 ms fade in/out avoids clicks */
            int edge = RATE / 100;
            float gain = i < edge ? (float)i / edge : (i > SAMPLES - edge ? (float)(SAMPLES - i) / edge : 1.0f);
            tone[i] = (int16_t)(24000.0f * gain * sinf(2.0f * (float)M_PI * TONE_HZ * i / RATE));
        }
        esp_codec_dev_write(s_spk, silence, RATE * LEAD_MS / 1000 * sizeof(int16_t));
        const int ret = esp_codec_dev_write(s_spk, tone, SAMPLES * sizeof(int16_t));
        esp_codec_dev_write(s_spk, silence, SILENCE * sizeof(int16_t));
        ESP_LOGI(TAG, "beep at %d%%: write %d", s_volume.load(), ret);
        esp_codec_dev_close(s_spk);
    } else {
        ESP_LOGW(TAG, "Speaker not available");
    }
    heap_caps_free(tone);
    heap_caps_free(silence);
    s_busy = false;
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void phone_audio_set_volume(int percent)
{
    s_volume = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

extern "C" int phone_audio_get_volume(void)
{
    return s_volume;
}

namespace {

std::atomic<phone_audio_yield_cb_t> s_yield;   // of the current holder, or null

void *take(phone_audio_yield_cb_t yield_cb)
{
    if (s_busy.exchange(true)) {
        /* a holder that can stop is asked to; otherwise (a beep of 150 ms, video, assistant) only a
         * short wait for the beep */
        const phone_audio_yield_cb_t yield = s_yield.load();
        if (yield) {
            ESP_LOGI(TAG, "speaker busy: asking its holder to stop");
            yield();
        }
        const int tries = yield ? 150 : 25;
        bool got = false;
        for (int i = 0; i < tries && !got; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
            got = !s_busy.exchange(true);
        }
        if (!got) {
            ESP_LOGW(TAG, "speaker busy");
            return nullptr;
        }
    }
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();
    }
    if (!s_spk) {
        s_busy = false;
        return nullptr;
    }
    s_yield = yield_cb;
    return s_spk;
}

} // namespace

extern "C" void *phone_audio_speaker_take(void)
{
    return take(nullptr);
}

extern "C" void *phone_audio_speaker_take_yielding(phone_audio_yield_cb_t yield_cb)
{
    return take(yield_cb);
}

extern "C" void phone_audio_speaker_give(void)
{
    s_yield = nullptr;
    s_busy = false;
}

extern "C" void phone_audio_beep(void)
{
    if (s_volume.load() == 0 || s_busy.exchange(true)) {
        return;
    }
    if (xTaskCreate(beep_task, "beep", 4096, nullptr, 4, nullptr) != pdPASS) {
        s_busy = false;
    }
}
