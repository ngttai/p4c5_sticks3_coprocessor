#include "radio_stations.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "phone_paths.h"
#include "phone_storage.h"
#include "sdkconfig.h"

namespace radio {

namespace {

const char *TAG = "radio_list";
const char *PATH = PHONE_SD_RADIO_STATIONS;
constexpr long MAX_FILE = 64 * 1024;
constexpr size_t MAX_STATIONS = 100;

bool usable_url(const char *url)
{
    return url && (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0);
}

} // namespace

ListResult load_stations(std::vector<Station> &out)
{
    out.clear();
    if (phone_storage_get(nullptr, nullptr) != PHONE_STORAGE_MOUNTED) {
        return ListResult::NO_CARD;
    }
    struct stat st = {};
    PATH = PHONE_SD_RADIO_STATIONS;
    if (stat(PATH, &st) != 0) {
        /* v0.1.0 kept the list at radio/stations.json */
        PATH = PHONE_SD_OLD_RADIO_STATIONS;
        if (stat(PATH, &st) != 0) {
            return ListResult::NO_FILE;
        }
        ESP_LOGW(TAG, "using %s: its place is now %s", PATH, PHONE_SD_RADIO_STATIONS);
    }
    if (st.st_size <= 0 || st.st_size > MAX_FILE) {
        ESP_LOGW(TAG, "%s: %ld bytes", PATH, static_cast<long>(st.st_size));
        return ListResult::BAD_FILE;
    }
    FILE *f = fopen(PATH, "rb");
    if (!f) {
        return ListResult::NO_FILE;
    }
    auto *text = static_cast<char *>(heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM));
    const size_t n = text ? fread(text, 1, st.st_size, f) : 0;
    fclose(f);
    if (!text || n != static_cast<size_t>(st.st_size)) {
        free(text);
        return ListResult::BAD_FILE;
    }
    text[n] = '\0';
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        ESP_LOGW(TAG, "%s: not valid JSON", PATH);
        return ListResult::BAD_FILE;
    }
    const cJSON *list = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "stations");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "%s: no \"stations\" array", PATH);
        return ListResult::BAD_FILE;
    }
    int skipped = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(item, "url");
        if (!cJSON_IsString(name) || !name->valuestring[0] || !cJSON_IsString(url) || !usable_url(url->valuestring)) {
            skipped++;
            continue;
        }
        if (out.size() >= MAX_STATIONS) {
            skipped++;
            continue;
        }
        out.push_back({name->valuestring, url->valuestring});
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%u stations from %s (%d skipped)", static_cast<unsigned>(out.size()), PATH, skipped);
    return out.empty() ? ListResult::EMPTY : ListResult::OK;
}

} // namespace radio
