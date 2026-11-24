#include "assistant_wake.h"

#include "esp_log.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"

#define TAG "assistant_wake"

namespace {

srmodel_list_t *s_models;
const esp_wn_iface_t *s_wn;
model_iface_data_t *s_data;
int s_chunk;

void close_now()
{
    if (s_data && s_wn) {
        s_wn->destroy(s_data);
    }
    s_data = nullptr;
    s_wn = nullptr;
    s_chunk = 0;
    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
    }
}

bool open_now()
{
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGW(TAG, "no model partition");
        return false;
    }
    const char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, nullptr);
    if (!name) {
        ESP_LOGW(TAG, "no wake word model in the partition");
        close_now();
        return false;
    }
    s_wn = esp_wn_handle_from_name(name);
    s_data = s_wn ? s_wn->create(name, DET_MODE_90) : nullptr;
    if (!s_data || s_wn->get_samp_rate(s_data) != 16000 || s_wn->get_channel_num(s_data) != 1) {
        ESP_LOGW(TAG, "model %s cannot be used", name);
        close_now();
        return false;
    }
    s_chunk = s_wn->get_samp_chunksize(s_data);
    ESP_LOGI(TAG, "wake word \"%s\" (%s), %d samples per chunk", esp_wn_wakeword_from_name(name), name, s_chunk);
    return true;
}

/* Mapping the model partition (esp_srmodel_init / deinit) turns the flash cache off for a moment, which needs
 * a stack in internal RAM. The assistant task keeps its stack in PSRAM, so a short task with an internal stack
 * opens and closes the model. */
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
    if (xTaskCreate(job_task, "wake_model", 6144, &job, 5, nullptr) != pdPASS) {
        vSemaphoreDelete(job.done);
        return false;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
    return job.result;
}

} // namespace

extern "C" bool assistant_wake_open(void)
{
    return s_data || run_job(true);
}

extern "C" void assistant_wake_close(void)
{
    if (s_data || s_models) {
        run_job(false);
    }
}

extern "C" int assistant_wake_chunk(void)
{
    return s_chunk;
}

extern "C" srmodel_list_t *assistant_wake_models(void)
{
    return s_data ? s_models : nullptr;
}

extern "C" bool assistant_wake_feed(const int16_t *pcm)
{
    if (!s_data) {
        return false;
    }
    return s_wn->detect(s_data, const_cast<int16_t *>(pcm)) > 0;
}
