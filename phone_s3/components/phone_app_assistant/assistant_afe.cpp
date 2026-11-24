#include "sdkconfig.h"
#include "assistant_afe.h"

#include <atomic>
#include <cstring>

#include "assistant_wake.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "assistant_afe"

namespace {

afe_config_t *s_cfg;
const esp_afe_sr_iface_t *s_iface;
esp_afe_sr_data_t *s_afe;
int s_chunk;
int16_t *s_in;                  // interleaved input: mic, ref
std::atomic<bool> s_woke;
std::atomic<bool> s_run;
SemaphoreHandle_t s_fetch_done;

/* fetch() blocks until a chunk has been processed (echo cancellation, then WakeNet), so it has a task of its own */
void fetch_task(void *)
{
    while (s_run) {
        afe_fetch_result_t *r = s_iface->fetch_with_delay(s_afe, pdMS_TO_TICKS(100));
        if (r && r->ret_value == ESP_OK && r->wakeup_state == WAKENET_DETECTED) {
            s_woke = true;
        }
    }
    xSemaphoreGive(s_fetch_done);
    vTaskDeleteWithCaps(nullptr);
}

size_t dma_free()
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

void close_now()
{
    if (s_run && s_fetch_done) {
        s_run = false;
        xSemaphoreTake(s_fetch_done, pdMS_TO_TICKS(1000));      // the task leaves within one fetch timeout
    }
    if (s_fetch_done) {
        vSemaphoreDelete(s_fetch_done);
        s_fetch_done = nullptr;
    }
    if (s_afe && s_iface) {
        s_iface->destroy(s_afe);
    }
    s_afe = nullptr;
    s_iface = nullptr;
    if (s_cfg) {
        afe_config_free(s_cfg);
        s_cfg = nullptr;
    }
    heap_caps_free(s_in);
    s_in = nullptr;
    s_chunk = 0;
}

bool open_now()
{
    srmodel_list_t *models = assistant_wake_models();
    if (!models) {
        ESP_LOGW(TAG, "no models (wake word off?)");
        return false;
    }
    const size_t sram0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), dma0 = dma_free();
    s_cfg = afe_config_init("MR", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!s_cfg) {
        ESP_LOGW(TAG, "afe_config_init failed");
        return false;
    }
    s_cfg->aec_init = true;
    s_cfg->se_init = false;
    s_cfg->ns_init = false;            // noise suppression keeps WakeNet from hearing "Hi ESP" (seen in stage 3b)
    s_cfg->vad_init = false;
    s_cfg->agc_init = false;
    s_cfg->wakenet_init = true;
    s_cfg->wakenet_mode = DET_MODE_90;
    s_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_cfg->afe_perferred_core = 1;
    s_cfg->afe_perferred_priority = 5;
    s_cfg = afe_config_check(s_cfg);
    s_iface = s_cfg ? esp_afe_handle_from_config(s_cfg) : nullptr;
    s_afe = s_iface ? s_iface->create_from_config(s_cfg) : nullptr;
    if (!s_afe) {
        ESP_LOGW(TAG, "AFE could not be created");
        close_now();
        return false;
    }
    s_chunk = s_iface->get_feed_chunksize(s_afe);
    const int channels = s_iface->get_feed_channel_num(s_afe);
    s_in = static_cast<int16_t *>(heap_caps_malloc(s_chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_fetch_done = xSemaphoreCreateBinary();
    s_run = true;
    if (channels != 2 || !s_in || !s_fetch_done ||
            xTaskCreatePinnedToCoreWithCaps(fetch_task, "assistant_afe", 6144, nullptr, 5, nullptr, 1,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_run = false;
        ESP_LOGW(TAG, "front-end not usable: %d channels", channels);
        close_now();
        return false;
    }
    s_iface->print_pipeline(s_afe);
    /* fetch() waits while nothing is fed (the assistant is not speaking): "Ringbuffer of AFE is empty" every 100 ms */
    esp_log_level_set("AFE", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "open: %d samples x %d channels, wake word %s; SRAM %u -> %u, DMA %u -> %u (largest %u)", s_chunk,
             channels, s_cfg->wakenet_model_name ? s_cfg->wakenet_model_name : "?", static_cast<unsigned>(sram0),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), static_cast<unsigned>(dma0),
             static_cast<unsigned>(dma_free()),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
    return true;
}

/* Like assistant_wake: the model work runs on a short task with an internal stack (the assistant task has its
 * stack in PSRAM) */
struct Job {
    bool open;
    bool result;
    SemaphoreHandle_t done;
};

void job_task(void *arg)
{
    auto *job = static_cast<Job *>(arg);
    if (job->open) {
        job->result = open_now();
    } else {
        close_now();
    }
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

bool run_job(bool open)
{
    Job job = {open, false, xSemaphoreCreateBinary()};
    if (!job.done) {
        return false;
    }
    if (xTaskCreate(job_task, "afe_model", 8192, &job, 5, nullptr) != pdPASS) {
        vSemaphoreDelete(job.done);
        return false;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.result;
}

} // namespace

extern "C" bool assistant_afe_open(void)
{
#ifdef CONFIG_BSP_AUDIO_S3_LINK
    /* The microphone is the S3's and already comes out of its AFE (AEC, NS): no AFE (nor wake word) on the P4 */
    return false;
#endif
    return s_afe || run_job(true);
}

extern "C" void assistant_afe_close(void)
{
    if (s_afe || s_cfg) {
        run_job(false);
    }
}

extern "C" int assistant_afe_chunk(void)
{
    return s_chunk;
}

extern "C" void assistant_afe_feed(const int16_t *mic, const int16_t *ref)
{
    if (!s_afe) {
        return;
    }
    for (int i = 0; i < s_chunk; i++) {
        s_in[i * 2] = mic[i];
        s_in[i * 2 + 1] = ref[i];
    }
    s_iface->feed(s_afe, s_in);
}

extern "C" bool assistant_afe_woke(void)
{
    return s_woke.exchange(false);
}

extern "C" void assistant_afe_reset(void)
{
    if (s_afe) {
        s_iface->reset_buffer(s_afe);
    }
    s_woke = false;
}
