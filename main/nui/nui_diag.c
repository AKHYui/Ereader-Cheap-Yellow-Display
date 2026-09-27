#include "nui_diag.h"

#include "net_wifi.h"
#include "nui_ui.h"
#include "nui_sleep.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = PV_TAG;

#define ROW_Y0      58
#define ROW_STEP    20
#define LABEL_X      8
#define VALUE_RX   232

#define FOOT_Y     272
#define FOOT_H      48
#define FOOT_W     100
#define FOOT_X     ((PV_SCR_W - FOOT_W) / 2)

#define REFRESH_MS 1000

enum { DH_NONE = 0, DH_BACK = 1 };

typedef struct {
    char label[16];
    char value[40];
    uint16_t color;
} row_t;

static void fmt_bytes(uint64_t b, char *out, size_t n)
{
    if (b >= 1024ULL * 1024 * 1024) {
        snprintf(out, n, "%.1fG", (double)b / (1024.0 * 1024 * 1024));
    } else if (b >= 1024ULL * 1024) {
        snprintf(out, n, "%.0fM", (double)b / (1024.0 * 1024));
    } else {
        snprintf(out, n, "%.0fK", (double)b / 1024.0);
    }
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
    }
}

static int collect(row_t *r, int max)
{
    int n = 0;

#define ADD(lbl, col, ...)                                          \
    do {                                                            \
        if (n < max) {                                              \
            snprintf(r[n].label, sizeof(r[n].label), "%s", (lbl));   \
            snprintf(r[n].value, sizeof(r[n].value), __VA_ARGS__);   \
            r[n].color = (col);                                     \
            n++;                                                    \
        }                                                           \
    } while (0)

    ADD("heap free", NUI_FG, "%u B",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ADD("heap block", NUI_OK, "%u B",
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    ADD("heap low", NUI_WARN, "%u B",
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));

    ADD("stack main", NUI_FG, "%u B",
        (unsigned)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));

    nvs_stats_t ns;
    const bool nvs_ok = (nvs_get_stats(NULL, &ns) == ESP_OK);
    if (nvs_ok) {
        ADD("nvs free", ns.free_entries > 16 ? NUI_DIM : NUI_WARN, "%u/%u",
            (unsigned)ns.free_entries, (unsigned)ns.total_entries);
    } else {
        ADD("nvs free", NUI_ERR, "n/a");
    }

    uint64_t tot = 0, fr = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &tot, &fr) == ESP_OK && tot > 0) {
        char a[12], b[12];
        fmt_bytes(fr, a, sizeof(a));
        fmt_bytes(tot, b, sizeof(b));
        ADD("sd free", NUI_FG, "%s/%s", a, b);
    } else {
        ADD("sd free", NUI_DIM, "no card");
    }

    const int rssi = net_wifi_rssi();
    if (rssi != 0) {
        ADD("wifi rssi", rssi > -70 ? NUI_OK : NUI_WARN, "%d dBm", rssi);
    } else if (net_wifi_ap_on()) {
        ADD("wifi ap", NUI_FG, "%d client", net_wifi_ap_clients());
    } else {
        ADD("wifi", NUI_DIM, "off");
    }

    {
        const int64_t sec = esp_timer_get_time() / 1000000;
        ADD("uptime", NUI_FG, "%02d:%02d:%02d",
            (int)(sec / 3600), (int)((sec / 60) % 60), (int)(sec % 60));
    }

    ADD("reset", NUI_FG, "%s", reset_name(esp_reset_reason()));

    {
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        ADD("chip", NUI_DIM, "%d core %dMHz", ci.cores,
            (int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }

#undef ADD
    return n;
}

static void diag_draw(void)
{
    row_t r[12];
    const int n = collect(r, 12);

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "芯片体检");

        for (int i = 0; i < n; i++) {
            const int y = ROW_Y0 + i * ROW_STEP;
            nui_ascii_row(row, sy, LABEL_X, y, r[i].label, NUI_DIM);
            nui_ascii_row(row, sy, VALUE_RX - nui_ascii_w1(r[i].value), y,
                          r[i].value, r[i].color);
        }

        nui_button_row(row, sy, FOOT_X, FOOT_Y, FOOT_W, FOOT_H,
                       "返回", false);
    }

    pv_disp_page_end();
}

static int diag_hit(int x, int y, void *ctx)
{
    (void)ctx;
    if (x >= FOOT_X && x < FOOT_X + FOOT_W &&
        y >= FOOT_Y && y < FOOT_Y + FOOT_H) {
        return DH_BACK;
    }
    return DH_NONE;
}

void nui_diag_run(void)
{
    diag_draw();
    ESP_LOGI(TAG, "体检页就绪（空闲堆 %u，最大块 %u）",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    int64_t last = esp_timer_get_time() / 1000;

    for (;;) {

        if (nui_sleep_poll()) {
            diag_draw();
            continue;
        }

        const int64_t t = esp_timer_get_time() / 1000;
        if (t - last >= REFRESH_MS) {
            last = t;
            diag_draw();
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        if (pv_touch_hit(diag_hit, NULL) == DH_BACK) return;
    }
}
