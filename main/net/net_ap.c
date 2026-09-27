#include "net_ap.h"

#include "net_httpd.h"
#include "net_qr.h"
#include "net_wifi.h"
#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = PV_TAG;

#define FOOT_Y      272
#define FOOT_H       48
#define BTN_W        72
#define BTN_GAP       4

#define POLL_MS     300

enum { AH_NONE = 0, AH_TOGGLE = 1, AH_QR = 2, AH_BACK = 3 };

typedef struct {
    bool on;
    bool running;
    bool failed;
    int  clients;
    int  files;
    int  percent;
    char ssid[NET_WIFI_SSID_LEN];
    char ip[16];
} ap_state_t;

static ap_state_t s_shown;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int btn_x(int which) { return 8 + (which - AH_TOGGLE) * (BTN_W + BTN_GAP); }

static void ap_draw(int pressed, const ap_state_t *st)
{
    char buf[48];

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "AP模式");

        if (!st->on) {
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 120, 40,
                                st->failed ? "连接失败" : "未开启",
                                st->failed ? NUI_ERR : NUI_WARN, NUI_BG);
        } else {

            snprintf(buf, sizeof(buf), "SSID %s", st->ssid);
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 56, 40, buf, NUI_FG, NUI_BG);

            snprintf(buf, sizeof(buf), "密码 %s", NET_WIFI_AP_PASS);
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 96, 40, buf, NUI_FG, NUI_BG);

            if (st->ip[0]) {
                nui_ascii2x_row(row, sy,
                                (PV_SCR_W - nui_ascii2x_w(st->ip)) / 2, 142,
                                st->ip, NUI_OK);
            }

            snprintf(buf, sizeof(buf), "已连接 %d 台", st->clients);
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 186, 40, buf,
                                st->clients ? NUI_OK : NUI_DIM, NUI_BG);

            if (st->percent >= 0) {
                snprintf(buf, sizeof(buf), "接收中 %d%%", st->percent);
                nui_text_mid_center(row, sy, PV_SCR_W / 2, 226, 40, buf, NUI_FG, NUI_BG);
            } else if (st->files > 0) {
                snprintf(buf, sizeof(buf), "已接收 %d 个", st->files);
                nui_text_mid_center(row, sy, PV_SCR_W / 2, 226, 40, buf, NUI_DIM, NUI_BG);
            }
        }

        nui_button_row(row, sy, btn_x(AH_TOGGLE), FOOT_Y, BTN_W, FOOT_H,
                       st->on ? "关闭" : "开启", pressed == AH_TOGGLE);
        nui_button_row(row, sy, btn_x(AH_QR), FOOT_Y, BTN_W, FOOT_H,
                       "扫码", pressed == AH_QR);
        nui_button_row(row, sy, btn_x(AH_BACK), FOOT_Y, BTN_W, FOOT_H,
                       "返回", pressed == AH_BACK);
    }

    pv_disp_page_end();
}

static void read_state(ap_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->on   = net_wifi_ap_on();
    st->ssid[0] = 0;
    st->ip[0]   = 0;

    if (st->on) {
        strncpy(st->ssid, net_wifi_ap_ssid(), sizeof(st->ssid) - 1);
        strncpy(st->ip, net_wifi_ap_ip(), sizeof(st->ip) - 1);
        st->clients = net_wifi_ap_clients();
    }

    st->running = net_httpd_running();

    net_httpd_prog_t p;
    net_httpd_prog(&p);
    st->files   = p.files;
    st->percent = p.percent;
}

static bool state_changed(const ap_state_t *a, const ap_state_t *b)
{
    return a->on      != b->on      ||
           a->running != b->running ||
           a->clients != b->clients ||
           a->files   != b->files   ||
           a->percent != b->percent ||
           strcmp(a->ssid, b->ssid) != 0 ||
           strcmp(a->ip, b->ip) != 0;
}

static int ap_hit(int x, int y, void *ctx)
{
    (void)ctx;
    if (y < FOOT_Y || y >= FOOT_Y + FOOT_H) return AH_NONE;

    for (int w = AH_TOGGLE; w <= AH_BACK; w++) {
        const int bx = btn_x(w);
        if (x >= bx && x < bx + BTN_W) return w;
    }
    return AH_NONE;
}

static bool ap_start_once(void)
{
    char ssid[NET_WIFI_SSID_LEN];
    net_wifi_ap_default_ssid(ssid, sizeof(ssid));

    if (net_wifi_ap_start(ssid, NET_WIFI_AP_PASS) != ESP_OK) {
        ESP_LOGE(TAG, "AP 启动失败（内存不够？看上面的日志）");
        return false;
    }
    ESP_LOGI(TAG, "AP 模式已就绪：%s / %s / %s",
             ssid, NET_WIFI_AP_PASS, net_wifi_ap_ip());
    return true;
}

static void ensure_service(ap_state_t *st)
{
    if ((st->on || net_wifi_connected()) && !st->running) {
        net_httpd_start();
        read_state(st);
    }
}

void net_ap_run(void)
{
    int  pressed = AH_NONE;
    bool back    = false;

    bool failed = false;
    if (!net_wifi_ap_on()) failed = !ap_start_once();

    read_state(&s_shown);
    ensure_service(&s_shown);
    s_shown.failed = failed;

    ap_draw(pressed, &s_shown);
    ESP_LOGI(TAG, "AP 页就绪（AP %s，IP %s，服务 %s）",
             s_shown.on ? "开" : "关", s_shown.ip[0] ? s_shown.ip : "无",
             s_shown.running ? "已启动" : "未启动");

    int64_t last_poll = 0;

    while (!back) {

        const int64_t t = now_ms();
        if (t - last_poll >= POLL_MS) {
            last_poll = t;

            ap_state_t cur;
            read_state(&cur);
            ensure_service(&cur);
            cur.failed = s_shown.failed;

            if (state_changed(&cur, &s_shown)) {
                s_shown = cur;
                ap_draw(pressed, &s_shown);
            }
        }

        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(ap_hit, NULL);
            if (h != pressed) {
                pressed = h;
                ap_draw(pressed, &s_shown);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(ap_hit, NULL);
        pressed = AH_NONE;

        if (h == AH_BACK) {
            back = true;
            continue;
        }

        if (h == AH_QR) {

            net_qr_run();
            read_state(&s_shown);
            ensure_service(&s_shown);
            s_shown.failed = failed;
            ap_draw(pressed, &s_shown);
            continue;
        }

        if (h == AH_TOGGLE) {
            bool f = false;
            if (s_shown.on) {
                net_wifi_ap_stop();
            } else {
                f = !ap_start_once();
            }

            read_state(&s_shown);
            ensure_service(&s_shown);
            s_shown.failed = f;
            ap_draw(pressed, &s_shown);
            continue;
        }

        ap_draw(pressed, &s_shown);
    }
}

#if NET_AP_SELFTEST

void net_ap_selftest(void)
{
    ESP_LOGI(TAG, "=== AP 模式自检 ===");
    net_wifi_mem("AP 自检前");

    char ssid[NET_WIFI_SSID_LEN];
    net_wifi_ap_default_ssid(ssid, sizeof(ssid));

    const int64_t t0 = esp_timer_get_time();
    const esp_err_t r = net_wifi_ap_start(ssid, NET_WIFI_AP_PASS);
    ESP_LOGI(TAG, "开 AP(%s / %s)：%s，用时 %dms",
             ssid, NET_WIFI_AP_PASS, esp_err_to_name(r),
             (int)((esp_timer_get_time() - t0) / 1000));
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "AP 起不来 —— 见上面 net_wifi 的日志");
        return;
    }

    for (int i = 0; i < 25 && !net_wifi_ap_ip()[0]; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "AP 状态：on=%d  ssid=%s  ip=%s  客户端=%d",
             (int)net_wifi_ap_on(), net_wifi_ap_ssid(), net_wifi_ap_ip(),
             net_wifi_ap_clients());
    net_wifi_mem("AP 自检后");
    ESP_LOGI(TAG, "=== AP 自检结束 ===");
}
#endif
