#include "net_remote.h"

#include "lcd_st7789.h"
#include "net_httpd.h"
#include "net_wifi.h"
#include "nui_qr.h"
#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"
#include "rd_font.h"
#include "rd_list.h"
#include "rd_txt.h"
#include "rd_view.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = PV_TAG;

#define QR_WHITE    0xFFFF
#define QR_BLACK    0x0000

#define QR_MOD        5
#define QR_QUIET      4
#define QR_Y         50

#define ROW1_Y      222
#define ROW2_Y      240
#define ROW3_Y      258

#define FOOT_Y      272
#define FOOT_H       48
#define FOOT_W      100
#define FOOT_X      ((PV_SCR_W - FOOT_W) / 2)

#define POLL_MS     300

enum { RH_NONE = 0, RH_BACK = 1 };

typedef struct {
    uint8_t cmd;
    int     arg;
} rc_msg_t;

#define RC_Q_LEN   8

static QueueHandle_t s_q;
static char          s_url[48];

static char s_open_path[RD_LIST_DIR_LEN + 1 + 256];

static net_remote_book_t s_book;

void net_remote_init(void)
{
    if (!s_q) s_q = xQueueCreate(RC_Q_LEN, sizeof(rc_msg_t));
}

bool net_remote_post(rc_cmd_t cmd, int arg)
{
    if (!s_q) {

        return false;
    }
    const rc_msg_t m = { .cmd = (uint8_t)cmd, .arg = arg };
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "遥控命令队列满，丢弃 cmd=%d", (int)cmd);
        return false;
    }
    return true;
}

bool net_remote_take(rc_cmd_t *cmd, int *arg)
{
    if (!s_q) return false;

    rc_msg_t m;
    if (xQueueReceive(s_q, &m, 0) != pdTRUE) return false;
    if (cmd) *cmd = (rc_cmd_t)m.cmd;
    if (arg) *arg = m.arg;
    return true;
}

bool net_remote_set_open_path(const char *path)
{
    if (!path || !path[0]) return false;
    snprintf(s_open_path, sizeof(s_open_path), "%s", path);
    return true;
}

const char *net_remote_open_path(void) { return s_open_path; }

void net_remote_sync(const char *file, int page, int pages)
{

    if (file && file[0]) {
        if (strcmp(s_book.name, file) != 0) {
            snprintf(s_book.name, sizeof(s_book.name), "%s", file);
        }
    } else if (s_book.name[0]) {
        s_book.name[0] = 0;
    }
    s_book.page  = pages > 0 ? page : -1;
    s_book.pages = pages;
}

void net_remote_state(net_remote_book_t *out)
{
    if (out) *out = s_book;
}

static void build_qr(void)
{
    const char *ip = net_wifi_ap_on() ? net_wifi_ap_ip() : net_wifi_ip();
    if (!ip || !ip[0]) {
        s_url[0] = 0;
        ESP_LOGW(TAG, "遥控页：还没拿到 IP，二维码先空着");
        return;
    }
    snprintf(s_url, sizeof(s_url), "http://%s/r", ip);
    const int n = nui_qr_encode(s_url);
    ESP_LOGI(TAG, "遥控页二维码：%s -> %d 模块（掩码 %d）", s_url, n, nui_qr_mask());
}

static void draw_book_name(uint16_t *row, int sy, const char *name, int y_top)
{
    if (!name || !name[0]) return;

    static char     u8[300];
    static uint32_t cps[40];

    rd_name_utf8(name, u8, (int)sizeof(u8));
    const int n = rd_cp_from_utf8(u8, cps, 40);
    if (n <= 0) return;

    rd_font_row(row, sy, 8, y_top, cps, n, QR_BLACK, QR_WHITE);
}

static void remote_draw(void)
{
    net_remote_book_t b;
    net_remote_state(&b);

    const int size  = nui_qr_size();
    const int total = size > 0 ? (size + QR_QUIET * 2) * QR_MOD : 0;
    const int x0    = (PV_SCR_W - total) / 2;

    pv_disp_page_begin(QR_WHITE);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_text_mid_center(row, sy, PV_SCR_W / 2, 4, 40, "网页遥控",
                            QR_BLACK, QR_WHITE);

        if (size > 0 && sy >= QR_Y && sy < QR_Y + total) {
            const int mr = (sy - QR_Y) / QR_MOD - QR_QUIET;
            if (mr >= 0 && mr < size) {
                for (int c = 0; c < size; c++) {
                    if (!nui_qr_dark(mr, c)) continue;
                    const int x = x0 + (c + QR_QUIET) * QR_MOD;
                    nui_hspan(row, x, x + QR_MOD, QR_BLACK);
                }
            }
        }

        if (s_url[0]) {
            nui_ascii_row(row, sy, 8, ROW1_Y, s_url, QR_BLACK);
        } else {
            nui_ascii_row(row, sy, 8, ROW1_Y, "no wifi", NUI_ERR);
        }

        if (b.pages > 0) {
            draw_book_name(row, sy, b.name, ROW2_Y);

            char t[32];
            snprintf(t, sizeof(t), "page %d/%d", b.page + 1, b.pages);
            nui_ascii_row(row, sy, 8, ROW3_Y, t, QR_BLACK);
        } else {
            nui_ascii_row(row, sy, 8, ROW2_Y, "no book", NUI_DIM);
            char t[32];
            snprintf(t, sizeof(t), "%d client", net_wifi_ap_clients());
            nui_ascii_row(row, sy, 8, ROW3_Y, t,
                          net_wifi_ap_clients() ? QR_BLACK : NUI_DIM);
        }

        nui_frame_row(row, sy, FOOT_X, FOOT_Y, FOOT_W, FOOT_H, QR_WHITE, QR_BLACK, 2);
        nui_text_mid_center(row, sy, FOOT_X + FOOT_W / 2, FOOT_Y, FOOT_H,
                            "返回", QR_BLACK, QR_WHITE);
    }

    pv_disp_page_end();
}

static int remote_hit(int x, int y, void *ctx)
{
    (void)ctx;
    if (x >= FOOT_X && x < FOOT_X + FOOT_W &&
        y >= FOOT_Y && y < FOOT_Y + FOOT_H) {
        return RH_BACK;
    }
    return RH_NONE;
}

void net_remote_run(void)
{
    net_remote_init();

    if (!net_wifi_ap_on() && !net_wifi_connected()) {
        char ssid[NET_WIFI_SSID_LEN];
        net_wifi_ap_default_ssid(ssid, sizeof(ssid));
        const esp_err_t rc = net_wifi_ap_start(ssid, NET_WIFI_AP_PASS);
        ESP_LOGW(TAG, "遥控页开了热点 %s：%s", ssid, esp_err_to_name(rc));

        for (int i = 0; i < 25 && !net_wifi_ap_ip()[0]; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (!net_httpd_running()) net_httpd_start();

    build_qr();
    net_remote_sync(NULL, 0, 0);

    remote_draw();
    ESP_LOGI(TAG, "遥控页就绪（%s，空闲堆 %u / 最大块 %u）", s_url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    int64_t last = esp_timer_get_time() / 1000;

    for (;;) {

        rc_cmd_t cmd;
        int      arg = 0;
        if (net_remote_take(&cmd, &arg)) {
            if (cmd == RC_OPEN) {
                const char *p = net_remote_open_path();
                if (p && p[0]) {
                    ESP_LOGW(TAG, "手机请求打开：%s", p);
                    rd_view_run(p);
                    net_remote_sync(NULL, 0, 0);
                }
            } else if (cmd == RC_BRIGHT) {
                lcd_set_brightness(lcd_get_brightness() + arg);
            } else if (cmd == RC_BACK) {
                return;
            }
            remote_draw();
            continue;
        }

        const int64_t t = esp_timer_get_time() / 1000;
        if (t - last >= POLL_MS) {
            last = t;
            remote_draw();
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        if (pv_touch_hit(remote_hit, NULL) == RH_BACK) return;
        remote_draw();
    }
}
