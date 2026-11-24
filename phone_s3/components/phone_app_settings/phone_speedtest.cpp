#include "phone_speedtest.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace {

const char *TAG = "SPEEDTEST";
constexpr int64_t TEST_US = 10 * 1000 * 1000;
constexpr int LAN_PORT = 8080;
constexpr int ANNOUNCE_PORT = 8081;     // speed_server.py says "KSDIY-SPEED <port>" here every second
constexpr size_t BUF_SIZE = 16 * 1024;
constexpr uint64_t UP_BYTES = 1ULL << 30;           // announced length; the test stops long before
constexpr uint64_t NET_BYTES = 99000000;            // Cloudflare refuses 100 MB and more (403)
constexpr uint32_t INT = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t DMA = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;

std::mutex s_mtx;
phone_speed_state_t s_st;
phone_speed_target_t s_target;
char s_url[128];

void sample(uint64_t bytes, int64_t t0)
{
    const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - t0) / 1000);
    const uint32_t sram = heap_caps_get_free_size(INT);
    const uint32_t dma = heap_caps_get_free_size(DMA);
    std::lock_guard<std::mutex> lk(s_mtx);
    s_st.bytes = bytes;
    s_st.ms = ms;
    s_st.kbps = ms ? static_cast<uint32_t>(bytes * 8 / ms) : 0;
    s_st.sram_min = std::min(s_st.sram_min, sram);
    s_st.dma_min = std::min(s_st.dma_min, dma);
}

bool make_url(bool upload)
{
    if (s_target == PHONE_SPEED_NET) {
        snprintf(s_url, sizeof(s_url), upload ? "http://speed.cloudflare.com/__up"
                 : "http://speed.cloudflare.com/__down?bytes=99000000");
        std::lock_guard<std::mutex> lk(s_mtx);
        snprintf(s_st.host, sizeof(s_st.host), "speed.cloudflare.com");
        return true;
    }
    /* The PC announces itself by UDP broadcast; without it, the gateway (a PC hotspot) */
    char host[20] = "";
    int port = LAN_PORT;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ANNOUNCE_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        timeval tv = {2, 500000};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            char msg[32];
            sockaddr_in from = {};
            socklen_t len = sizeof(from);
            const int n = recvfrom(sock, msg, sizeof(msg) - 1, 0, reinterpret_cast<sockaddr *>(&from), &len);
            if (n > 12 && strncmp(msg, "KSDIY-SPEED ", 12) == 0) {
                msg[n] = '\0';
                port = atoi(msg + 12);
                inet_ntoa_r(from.sin_addr, host, sizeof(host));
            }
        }
        close(sock);
    }
    if (!host[0]) {
        esp_netif_t *netif = esp_netif_get_default_netif();
        esp_netif_ip_info_t ip = {};
        if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.gw.addr == 0) {
            return false;
        }
        snprintf(host, sizeof(host), IPSTR, IP2STR(&ip.gw));
        port = LAN_PORT;
    }
    snprintf(s_url, sizeof(s_url), "http://%s:%d/%s", host, port, upload ? "up" : "down");
    std::lock_guard<std::mutex> lk(s_mtx);
    snprintf(s_st.host, sizeof(s_st.host), "%s:%d", host, port);
    return true;
}

bool run(bool upload, char *buf)
{
    if (!make_url(upload)) {
        ESP_LOGW(TAG, "no gateway address");
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = s_url;
    cfg.method = upload ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = 5000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    ESP_LOGI(TAG, "%s %s", upload ? "upload to" : "download from", s_url);
    bool ok = false;
    uint64_t total = 0;
    int64_t t0 = 0;
    if (upload) {
        esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
        if (esp_http_client_open(c, static_cast<int>(s_target == PHONE_SPEED_NET ? NET_BYTES : std::min<uint64_t>(UP_BYTES, INT32_MAX))) == ESP_OK) {
            memset(buf, 0x5A, BUF_SIZE);
            t0 = esp_timer_get_time();
            ok = true;
            while (esp_timer_get_time() - t0 < TEST_US) {
                const int n = esp_http_client_write(c, buf, BUF_SIZE);
                if (n <= 0) {
                    ok = total > 0;
                    break;
                }
                total += n;
                sample(total, t0);
            }
        }
    } else if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        const int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            t0 = esp_timer_get_time();
            ok = true;
            while (esp_timer_get_time() - t0 < TEST_US) {
                const int n = esp_http_client_read(c, buf, BUF_SIZE);
                if (n <= 0) {
                    ok = total > 0;
                    break;
                }
                total += n;
                sample(total, t0);
            }
        } else {
            ESP_LOGW(TAG, "HTTP %d", status);
        }
    }
    if (t0) {
        sample(total, t0);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

void task(void *arg)
{
    const bool upload = arg != nullptr;
    char *buf = static_cast<char *>(heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM));
    const bool ok = buf && run(upload, buf);
    free(buf);
    phone_speed_state_t st;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_st.failed = !ok;
        s_st.running = false;
        st = s_st;
    }
    if (ok) {
        ESP_LOGI(TAG, "%s %s: %u.%02u Mbps (%llu bytes in %u ms) | sram min %uK | dma min %uK",
                 upload ? "upload" : "download", st.host, static_cast<unsigned>(st.kbps / 1000),
                 static_cast<unsigned>(st.kbps % 1000 / 10), static_cast<unsigned long long>(st.bytes),
                 static_cast<unsigned>(st.ms), static_cast<unsigned>(st.sram_min / 1024),
                 static_cast<unsigned>(st.dma_min / 1024));
    } else {
        ESP_LOGW(TAG, "%s %s failed", upload ? "upload" : "download", st.host);
    }
    vTaskDeleteWithCaps(nullptr);
}

} // namespace

extern "C" bool phone_speedtest_start(phone_speed_target_t target, bool upload)
{
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_st.running) {
            return false;
        }
        s_st = {};
        s_st.running = true;
        s_st.upload = upload;
        s_st.sram_min = UINT32_MAX;
        s_st.dma_min = UINT32_MAX;
    }
    s_target = target;
    /* in PSRAM: keeps the test out of the internal RAM it measures */
    if (xTaskCreatePinnedToCoreWithCaps(task, "speedtest", 8192, upload ? reinterpret_cast<void *>(1) : nullptr, 4,
                                        nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_st.running = false;
        s_st.failed = true;
        return false;
    }
    return true;
}

extern "C" void phone_speedtest_get(phone_speed_state_t *out)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    *out = s_st;
}
