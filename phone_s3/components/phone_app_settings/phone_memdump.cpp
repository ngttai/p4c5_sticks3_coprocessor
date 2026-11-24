/*
 * Internal RAM report for the log (tag MEMDUMP), made when the system monitor is turned on: the
 * stack of every task and where it is, the used blocks of the internal heap by size, the largest of
 * them, and what changed since the last report (blocks still held after an app is closed).
 */
#include "phone_monitor.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

const char *TAG = "MEMDUMP";
constexpr int MAX_BLOCKS = 6000;
constexpr int MAX_TASKS = 64;

struct Block {
    void *ptr;
    uint32_t size;
};

struct Snapshot {
    Block used[MAX_BLOCKS];
    int n_used;
    int n_free;
    uint32_t free_bytes;
    uint32_t free_big;
    bool overflow;
};

Snapshot *s_prev;           // in PSRAM
Snapshot *s_cur;

bool walker(walker_heap_into_t, walker_block_info_t b, void *user)
{
    auto *s = static_cast<Snapshot *>(user);
    if (b.used) {
        if (s->n_used < MAX_BLOCKS) {
            s->used[s->n_used++] = {b.ptr, static_cast<uint32_t>(b.size)};
        } else {
            s->overflow = true;
        }
    } else {
        s->n_free++;
        s->free_bytes += b.size;
        s->free_big = std::max<uint32_t>(s->free_big, b.size);
    }
    return true;
}

/* The used block that holds p, or -1 (blocks sorted by address) */
int find_block(const Snapshot *s, const void *p)
{
    int lo = 0, hi = s->n_used - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const auto *a = static_cast<const uint8_t *>(s->used[mid].ptr);
        if (p < a) {
            hi = mid - 1;
        } else if (p >= a + s->used[mid].size) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }
    return -1;
}

bool same(const Block &a, const Block &b)
{
    return a.ptr == b.ptr && a.size == b.size;
}

/* First bytes of a block, printable ones as they are: often enough to tell what it holds */
void peek(const Block &b, char *out)
{
    const auto *p = static_cast<const uint8_t *>(b.ptr);
    const int n = b.size < 16 ? static_cast<int>(b.size) : 16;
    for (int i = 0; i < n; i++) {
        out[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '.';
    }
    out[n] = '\0';
}

void report()
{
    if (!s_cur) {
        s_cur = static_cast<Snapshot *>(heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM));
        s_prev = static_cast<Snapshot *>(heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM));
        if (!s_cur || !s_prev) {
            ESP_LOGE(TAG, "no memory");
            return;
        }
    }
    auto *tasks = static_cast<TaskStatus_t *>(heap_caps_malloc(sizeof(TaskStatus_t) * MAX_TASKS, MALLOC_CAP_SPIRAM));
    auto *task_block = static_cast<int *>(heap_caps_malloc(sizeof(int) * MAX_TASKS, MALLOC_CAP_SPIRAM));
    auto *is_stack = static_cast<bool *>(heap_caps_calloc(MAX_BLOCKS, sizeof(bool), MALLOC_CAP_SPIRAM));
    if (!tasks || !task_block || !is_stack) {
        ESP_LOGE(TAG, "no memory");
        free(tasks);
        free(task_block);
        free(is_stack);
        return;
    }

    std::swap(s_prev, s_cur);
    memset(s_cur, 0, sizeof(Snapshot));
    const int n_tasks = static_cast<int>(uxTaskGetSystemState(tasks, MAX_TASKS, nullptr));
    heap_caps_walk(MALLOC_CAP_INTERNAL, walker, s_cur);
    Snapshot *s = s_cur;
    std::sort(s->used, s->used + s->n_used, [](const Block & a, const Block & b) {
        return a.ptr < b.ptr;
    });

    /* Tasks, internal stacks first, biggest first. The size comes from the heap block of the stack
     * (TaskStatus_t has no stack end here); a static stack and PSRAM stacks show 0 */
    uint32_t size[MAX_TASKS];
    int order[MAX_TASKS];
    for (int i = 0; i < n_tasks; i++) {
        const TaskStatus_t &t = tasks[i];
        order[i] = i;
        size[i] = 0;
        task_block[i] = -1;
        if (esp_ptr_internal(t.pxStackBase)) {
            const int bi = find_block(s, t.pxStackBase);
            task_block[i] = bi;
            if (bi >= 0 && s->used[bi].ptr == t.pxStackBase) {
                size[i] = s->used[bi].size;
                is_stack[bi] = true;
            }
        }
    }
    std::sort(order, order + n_tasks, [&](int a, int b) {
        const bool ai = esp_ptr_internal(tasks[a].pxStackBase), bi = esp_ptr_internal(tasks[b].pxStackBase);
        return ai != bi ? ai : size[a] > size[b];
    });
    uint32_t stack_int = 0;
    ESP_LOGI(TAG, "---- tasks (%d): name, stack size, never used, where", n_tasks);
    for (int k = 0; k < n_tasks; k++) {
        const int i = order[k];
        const TaskStatus_t &t = tasks[i];
        const bool internal = esp_ptr_internal(t.pxStackBase);
        const int bi = task_block[i];
        stack_int += size[i];
        ESP_LOGI(TAG, "  %-16s %6u %6u %s%s", t.pcTaskName, static_cast<unsigned>(size[i]),
                 static_cast<unsigned>(t.usStackHighWaterMark), internal ? "SRAM" : "PSRAM",
                 internal ? (bi < 0 ? " static" : (s->used[bi].ptr == t.pxStackBase ? "" : " in block")) : "");
    }

    /* Used blocks by size */
    constexpr uint32_t BIN[] = {64, 256, 1024, 4096, 16384, UINT32_MAX};
    uint32_t bin_n[6] = {}, bin_b[6] = {};
    uint32_t used_bytes = 0;
    for (int i = 0; i < s->n_used; i++) {
        used_bytes += s->used[i].size;
        if (is_stack[i]) {
            continue;
        }
        int k = 0;
        while (s->used[i].size >= BIN[k]) {
            k++;
        }
        bin_n[k]++;
        bin_b[k] += s->used[i].size;
    }
    ESP_LOGI(TAG, "---- internal heap: used %u in %d blocks (task stacks %u), free %u in %d blocks, largest %u%s",
             static_cast<unsigned>(used_bytes), s->n_used, static_cast<unsigned>(stack_int),
             static_cast<unsigned>(s->free_bytes), s->n_free, static_cast<unsigned>(s->free_big), s->overflow ? " (list full)" : "");
    ESP_LOGI(TAG, "  other blocks <64: %u/%u  <256: %u/%u  <1K: %u/%u  <4K: %u/%u  <16K: %u/%u  >=16K: %u/%u (count/bytes)",
             (unsigned)bin_n[0], (unsigned)bin_b[0], (unsigned)bin_n[1], (unsigned)bin_b[1], (unsigned)bin_n[2], (unsigned)bin_b[2],
             (unsigned)bin_n[3], (unsigned)bin_b[3], (unsigned)bin_n[4], (unsigned)bin_b[4], (unsigned)bin_n[5], (unsigned)bin_b[5]);

    /* The largest blocks that are not stacks */
    Block big[24];
    int n_big = 0;
    for (int i = 0; i < s->n_used; i++) {
        if (is_stack[i]) {
            continue;
        }
        if (n_big < 24) {
            big[n_big++] = s->used[i];
        } else if (s->used[i].size > big[23].size) {
            big[23] = s->used[i];
        } else {
            continue;
        }
        std::sort(big, big + n_big, [](const Block & a, const Block & b) {
            return a.size > b.size;
        });
    }
    ESP_LOGI(TAG, "---- largest other blocks: address, size, dma, first bytes");
    char text[17];
    for (int i = 0; i < n_big; i++) {
        peek(big[i], text);
        ESP_LOGI(TAG, "  %p %6u %s |%s|", big[i].ptr, static_cast<unsigned>(big[i].size),
                 esp_ptr_dma_capable(big[i].ptr) ? "dma" : "   ", text);
    }

    /* Since the last report: blocks taken and not given back (both lists sorted by address) */
    if (s_prev->n_used) {
        uint32_t new_n = 0, new_b = 0, gone_n = 0, gone_b = 0;
        int a = 0, b = 0;
        ESP_LOGI(TAG, "---- new since the last report (not stacks): address, size, first bytes");
        int shown = 0;
        while (a < s_prev->n_used || b < s->n_used) {
            if (b < s->n_used && (a >= s_prev->n_used || s->used[b].ptr < s_prev->used[a].ptr)) {
                new_n++;
                new_b += s->used[b].size;
                if (!is_stack[b] && s->used[b].size >= 256 && shown < 30) {
                    peek(s->used[b], text);
                    ESP_LOGI(TAG, "  %p %6u |%s|", s->used[b].ptr, static_cast<unsigned>(s->used[b].size), text);
                    shown++;
                }
                b++;
            } else if (a < s_prev->n_used && (b >= s->n_used || s_prev->used[a].ptr < s->used[b].ptr)) {
                gone_n++;
                gone_b += s_prev->used[a].size;
                a++;
            } else {
                if (!same(s_prev->used[a], s->used[b])) {
                    new_n++;
                    new_b += s->used[b].size;
                    gone_n++;
                    gone_b += s_prev->used[a].size;
                }
                a++;
                b++;
            }
        }
        ESP_LOGI(TAG, "  taken %u blocks, %u bytes; given back %u blocks, %u bytes; net %+d",
                 (unsigned)new_n, (unsigned)new_b, (unsigned)gone_n, (unsigned)gone_b,
                 static_cast<int>(new_b) - static_cast<int>(gone_b));
    }
    ESP_LOGI(TAG, "---- end");
    free(tasks);
    free(task_block);
    free(is_stack);
}

} // namespace

extern "C" void phone_memdump_report(void)
{
    report();
}
