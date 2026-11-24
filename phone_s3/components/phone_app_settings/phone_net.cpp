#include "phone_net.h"
#include "phone_locale.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "esp_hosted.h"

#define TAG "STAT"

namespace {

constexpr int POLL_MS = 5000;
constexpr uint64_t RETRY_US = 5000000;
constexpr uint64_t RETRY_DROP_US = 1000000;   // first retry after an established link was dropped
constexpr int SAME_TRIES = 3;                 // attempts on one network before moving to the next saved one
constexpr int MAX_SAVED = 5;
constexpr char NVS_NS[] = "phone";
constexpr char NVS_KEY[] = "wifi_aps";

/** One saved access point; password is empty for open networks. */
struct SavedAp {
    char ssid[33];
    char pass[65];
};

/** Saved networks, index 0 is the most recently used one. Persisted as one NVS blob. */
struct SavedList {
    int count;
    SavedAp ap[MAX_SAVED];
};

SemaphoreHandle_t s_mtx;
phone_net_status_t s_st;
SavedList s_list;                 // guarded by s_mtx
int s_try = 0;                    // index in s_list of the current attempt; guarded by s_mtx
int s_fail = 0;                   // consecutive failures on s_try; guarded by s_mtx
int s_other = 0;                  // the last other saved network tried (1..count-1); guarded by s_mtx
std::atomic<bool> s_ready{false};
char s_c5_ver[16];                // written once before s_c5_ver_ok is set
std::atomic<bool> s_c5_ver_ok{false};
std::atomic<bool> s_auto{false};
std::atomic<bool> s_enabled{true};    // Wi-Fi on for the user (Settings switch, quick settings tile)
char s_pend_ssid[33] = {};       // user-entered credentials, saved only after GOT_IP
char s_pend_pass[65] = {};
std::atomic<bool> s_user_op{false};
bool s_sntp_started;
esp_timer_handle_t s_retry;

struct Lock {
    Lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mtx); }
};

/* ---- saved list (caller holds s_mtx) ---- */

void list_load()
{
    memset(&s_list, 0, sizeof(s_list));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    SavedList tmp = {};
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, NVS_KEY, &tmp, &len) == ESP_OK && tmp.count >= 0 && tmp.count <= MAX_SAVED) {
        s_list = tmp;
    }
    nvs_close(h);
}

void list_save()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, NVS_KEY, &s_list, sizeof(s_list));
    nvs_commit(h);
    nvs_close(h);
}

int list_find(const char *ssid)
{
    for (int i = 0; i < s_list.count; i++) {
        if (strncmp(s_list.ap[i].ssid, ssid, sizeof(s_list.ap[i].ssid)) == 0) {
            return i;
        }
    }
    return -1;
}

/** Move entry i to the front; returns true when the order changed. */
bool list_touch(int i)
{
    if (i <= 0 || i >= s_list.count) {
        return false;
    }
    SavedAp tmp = s_list.ap[i];
    memmove(&s_list.ap[1], &s_list.ap[0], sizeof(SavedAp) * i);
    s_list.ap[0] = tmp;
    return true;
}

/** Insert or update a network at the front; when full, the oldest entry is replaced. */
void list_put(const char *ssid, const char *pass)
{
    int i = list_find(ssid);
    if (i < 0) {
        if (s_list.count < MAX_SAVED) {
            i = s_list.count;
            s_list.count++;
        } else {
            i = s_list.count - 1;
        }
    }
    strlcpy(s_list.ap[i].ssid, ssid, sizeof(s_list.ap[i].ssid));
    strlcpy(s_list.ap[i].pass, pass, sizeof(s_list.ap[i].pass));
    list_touch(i);
}

void list_remove(int i)
{
    for (int k = i; k < s_list.count - 1; k++) {
        s_list.ap[k] = s_list.ap[k + 1];
    }
    if (s_list.count > 0) {
        s_list.count--;
    }
    memset(&s_list.ap[s_list.count], 0, sizeof(SavedAp));
}

/** Fill cfg with saved entry idx and mark the attempt as connecting; false when idx is invalid. */
bool build_cfg(int idx, wifi_config_t *cfg)
{
    Lock l;
    if (idx < 0 || idx >= s_list.count) {
        return false;
    }
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(reinterpret_cast<char *>(cfg->sta.ssid), s_list.ap[idx].ssid, sizeof(cfg->sta.ssid));
    strlcpy(reinterpret_cast<char *>(cfg->sta.password), s_list.ap[idx].pass, sizeof(cfg->sta.password));
    cfg->sta.pmf_cfg.capable = true;
    s_st.wifi_connecting = true;
    strlcpy(s_st.ssid, s_list.ap[idx].ssid, sizeof(s_st.ssid));
    return true;
}

/** Start connecting to saved entry idx; must be called without s_mtx held. */
void start_attempt(int idx)
{
    s_pend_ssid[0] = '\0';
    s_pend_pass[0] = '\0';
    wifi_config_t cfg = {};
    if (build_cfg(idx, &cfg)) {
        ESP_LOGI(TAG, "Wifi try %d: %s", idx, reinterpret_cast<const char *>(cfg.sta.ssid));
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_connect();
    }
}

void on_time_sync(struct timeval *)
{
    Lock l;
    s_st.time_synced = true;
    ESP_LOGI(TAG, "Time synced");
}

void sntp_start_once()
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.wait_for_sync = false;
    cfg.sync_cb = on_time_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
    }
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (phone_pref_get("wifi", 1)) {        /* turned off before a restart: stays off */
                phone_net_enable();
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            bool retry;
            bool was_up;
            if (data) {
                /* why the link dropped: needed to tell a weak signal from a rejected or expired association */
                auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
                ESP_LOGW(TAG, "Wifi down: reason %d, rssi %d", ev->reason, ev->rssi);
            }
            {
                Lock l;
                retry = s_auto && !s_user_op && s_list.count > 0;
                was_up = s_st.wifi_connected;
                if (retry) {
                    /* a router drop of a working link is retried on the same network first; only
                     * repeated failures move on to the next saved one */
                    /* the last used network (index 0) gets most attempts: SAME_TRIES on it, then one on
                     * another saved network in turn, then back (a phone hotspot that comes back is found
                     * within a minute even with five networks saved) */
                    if (s_try != 0 || ++s_fail >= SAME_TRIES) {
                        s_fail = 0;
                        if (s_try == 0 && s_list.count > 1) {
                            s_other = s_other % (s_list.count - 1) + 1;
                            s_try = s_other;
                        } else {
                            s_try = 0;
                        }
                    }
                }
                s_st.wifi_connected = false;
                s_st.wifi_connecting = retry;
                s_st.rssi = 0;
            }
            if (retry) {
                esp_timer_start_once(s_retry, was_up ? RETRY_DROP_US : RETRY_US);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_PPP_GOT_IP) {
        sntp_start_once();          /* booted on 4G with no Wi-Fi: the clock still needs SNTP (TLS checks dates) */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        wifi_ap_record_t ap = {};
        bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
        s_user_op = false;
        s_auto = true;
        esp_timer_stop(s_retry);
        {
            Lock l;
            s_st.wifi_connected = true;
            s_st.wifi_connecting = false;
            snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&ev->ip_info.ip));
            if (have_ap) {
                strlcpy(s_st.ssid, reinterpret_cast<const char *>(ap.ssid), sizeof(s_st.ssid));
                s_st.rssi = ap.rssi;
                if (s_pend_ssid[0] && strcmp(s_pend_ssid, s_st.ssid) == 0) {
                    list_put(s_pend_ssid, s_pend_pass);
                    list_save();
                }
                int i = list_find(s_st.ssid);
                if (i >= 0 && list_touch(i)) {
                    list_save();
                }
            }
            s_pend_ssid[0] = '\0';
            s_pend_pass[0] = '\0';
            s_try = 0;
            s_fail = 0;
        }
        ESP_LOGI(TAG, "Wifi up %s", s_st.ip);
        sntp_start_once();
    }
}

void retry_cb(void *)
{
    int idx;
    {
        Lock l;
        /* A connect that succeeded after this retry was armed must not be torn down */
        if (!s_auto || s_user_op || s_list.count == 0 || s_st.wifi_connected) {
            return;
        }
        idx = s_try;
    }
    start_attempt(idx);
}

bool wifi_start()
{
    /* NVS, netif, event loop, SDIO link to the C5, esp_wifi_init/start */
    if (bsp_wifi_init() != ESP_OK) {
        return false;
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_event, nullptr);
    esp_hosted_coprocessor_fwver_t ver = {};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK) {
        snprintf(s_c5_ver, sizeof(s_c5_ver), "%u.%u.%u", (unsigned)ver.major1, (unsigned)ver.minor1, (unsigned)ver.patch1);
        s_c5_ver_ok = true;
    }
    /* the C5 keeps its settings across P4 resets: do not inherit a band restriction */
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    s_ready = true;
    ESP_LOGI(TAG, "Wifi started");
    /* STA_START fired inside bsp_wifi_init(), before the handlers above existed */
    if (phone_pref_get("wifi", 1)) {
        phone_net_enable();
    } else {
        s_enabled = false;
        ESP_LOGI(TAG, "Wifi off (switched off before the restart)");
    }
    return true;
}

void worker(void *)
{
    bool wifi_failed_logged = false;
    for (;;) {
        if (!s_ready) {
            if (!wifi_start() && !wifi_failed_logged) {
                ESP_LOGW(TAG, "Wifi start failed, retrying");
                wifi_failed_logged = true;
            }
        } else {
            bool connected;
            {
                Lock l;
                connected = s_st.wifi_connected;
            }
            /* Asking the C5 for AP info while not associated only logs an RPC error */
            wifi_ap_record_t ap = {};
            if (connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                Lock l;
                s_st.rssi = ap.rssi;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

} // namespace

extern "C" void phone_net_start(void)
{
    if (s_mtx) {
        return;
    }
    s_mtx = xSemaphoreCreateMutex();
    s_enabled = phone_pref_get("wifi", 1) != 0;   /* the switch before a restart; the tile shows it at once */
    {
        Lock l;
        list_load();
    }
    esp_timer_create_args_t args = {};
    args.callback = retry_cb;
    args.name = "wifi_retry";
    esp_timer_create(&args, &s_retry);
    xTaskCreate(worker, "stat", 3072, nullptr, 3, nullptr);
}

extern "C" void phone_net_get_status(phone_net_status_t *out)
{
    if (!out || !s_mtx) {
        return;
    }
    Lock l;
    *out = s_st;
}

extern "C" bool phone_net_online(void)
{
    phone_net_status_t st = {};
    phone_net_get_status(&st);
    if (st.wifi_connected) {
        return true;
    }
    return false;
}

extern "C" bool phone_net_ready(void)
{
    return s_ready;
}

extern "C" bool phone_net_c5_version(char *out, size_t len)
{
    if (!s_c5_ver_ok) {
        return false;
    }
    strlcpy(out, s_c5_ver, len);
    return true;
}

extern "C" bool phone_net_is_enabled(void)
{
    return s_enabled;
}

extern "C" void phone_net_disconnect(void)
{
    s_enabled = false;
    s_user_op = true;
    s_auto = false;
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    {
        Lock l;
        s_st.wifi_connecting = false;   /* no DISCONNECTED event when it was only trying */
    }
}

extern "C" void phone_net_pause_auto(void)
{
    s_user_op = true;
    s_auto = false;
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    Lock l;
    s_st.wifi_connecting = false;
}

extern "C" void phone_net_set_enabled(bool on)
{
    phone_pref_set("wifi", on ? 1 : 0);
    if (on) {
        phone_net_enable();
    } else {
        phone_net_disconnect();
    }
}

extern "C" void phone_net_enable(void)
{
    s_enabled = true;
    if (!s_ready) {
        return;
    }
    bool go;
    {
        Lock l;
        /* ESP-Hosted reports STA_START more than once: do not restart an attempt in progress */
        go = s_list.count > 0 && !s_st.wifi_connected && !s_st.wifi_connecting;
        if (go) {
            s_user_op = false;
            s_auto = true;
            s_try = 0;
            s_fail = 0;
        }
    }
    if (go) {
        start_attempt(0);
    }
}

extern "C" int phone_net_saved_count(void)
{
    Lock l;
    return s_list.count;
}

extern "C" bool phone_net_saved_at(int index, char *out, size_t len)
{
    Lock l;
    if (!out || index < 0 || index >= s_list.count) {
        return false;
    }
    strlcpy(out, s_list.ap[index].ssid, len);
    return true;
}

extern "C" bool phone_net_saved_ssid(char *out, size_t len)
{
    return phone_net_saved_at(0, out, len);
}

extern "C" void phone_net_forget_ssid(const char *ssid)
{
    if (!ssid || !s_mtx) {
        return;
    }
    bool active;
    {
        Lock l;
        int i = list_find(ssid);
        if (i < 0) {
            return;
        }
        active = (s_st.wifi_connected || s_st.wifi_connecting) &&
                 strncmp(s_st.ssid, ssid, sizeof(s_st.ssid)) == 0;
        list_remove(i);
        list_save();
        if (s_list.count == 0) {
            s_auto = false;
            s_try = 0;
        } else if (active) {
            // the disconnect event advances s_try by one, so point it at the first remaining entry
            s_try = s_list.count - 1;
            s_fail = SAME_TRIES - 1;
        } else if (s_try >= s_list.count) {
            s_try = 0;
        }
        if (active) {
            s_st.wifi_connected = false;
            s_st.wifi_connecting = false;
            s_st.ssid[0] = '\0';
        }
    }
    if (active) {
        esp_wifi_disconnect();
    }
}

extern "C" void phone_net_forget(void)
{
    char ssid[33] = {};
    if (phone_net_saved_ssid(ssid, sizeof(ssid))) {
        phone_net_forget_ssid(ssid);
    }
}

extern "C" void phone_net_connect_saved(int index)
{
    char ssid[33] = {};
    char pass[65] = {};
    {
        Lock l;
        if (index < 0 || index >= s_list.count) {
            return;
        }
        strlcpy(ssid, s_list.ap[index].ssid, sizeof(ssid));
        strlcpy(pass, s_list.ap[index].pass, sizeof(pass));
    }
    phone_net_connect(ssid, pass);
}

extern "C" void phone_net_connect(const char *ssid, const char *pass)
{
    if (!s_ready || !ssid) {
        return;
    }
    {
        Lock l;
        strlcpy(s_pend_ssid, ssid, sizeof(s_pend_ssid));
        strlcpy(s_pend_pass, pass ? pass : "", sizeof(s_pend_pass));
        s_try = 0;
        s_fail = 0;
        s_user_op = true;
        s_auto = false;
    }
    s_enabled = true;                   /* the user picked a network: Wi-Fi is on (4G goes back to standby) */
    phone_pref_set("wifi", 1);
    esp_timer_stop(s_retry);
    wifi_config_t cfg = {};
    {
        Lock l;
        strlcpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid));
        strlcpy(reinterpret_cast<char *>(cfg.sta.password), pass ? pass : "", sizeof(cfg.sta.password));
        cfg.sta.pmf_cfg.capable = true;
        s_st.wifi_connecting = true;
        strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
    }
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
}
