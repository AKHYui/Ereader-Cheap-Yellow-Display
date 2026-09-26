#include "net_recv.h"

#include "net_httpd.h"
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
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>

static const char *TAG = PV_TAG;

#define FOOT_Y   272
#define FOOT_H    48

#define POLL_MS  300

enum { RH_NONE = 0, RH_BACK = 1 };

typedef struct {
    bool conn;
    bool ap;
    bool running;
    int  files;
    int  percent;
    char ip[16];
} recv_state_t;

static recv_state_t s_shown;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void recv_draw(int pressed, const recv_state_t *st)
{
    char buf[40];

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "文件接收");

        if (!st->conn && !st->ap) {

            nui_text_mid_center(row, sy, PV_SCR_W / 2, 90, 40,
                                "请先连接 WLAN", NUI_WARN, NUI_BG);
        } else if (!st->running) {
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 90, 40,
                                "连接失败", NUI_ERR, NUI_BG);
        } else {

            nui_ascii2x_row(row, sy, (PV_SCR_W - nui_ascii2x_w(st->ip)) / 2, 76,
                            st->ip, NUI_OK);

            if (st->percent >= 0) {
                snprintf(buf, sizeof(buf), "接收中 %d%%", st->percent);
                nui_text_mid_center(row, sy, PV_SCR_W / 2, 140, 40,
                                    buf, NUI_FG, NUI_BG);
            } else {
                snprintf(buf, sizeof(buf), "已接收 %d 个", st->files);
                nui_text_mid_center(row, sy, PV_SCR_W / 2, 140, 40,
                                    buf, NUI_DIM, NUI_BG);
            }
        }

        nui_button_row(row, sy, 82, FOOT_Y, 76, FOOT_H, "返回", pressed == RH_BACK);
    }

    pv_disp_page_end();
}

static void read_state(recv_state_t *st)
{
    st->conn = net_wifi_connected();
    st->ap   = net_wifi_ap_on();

    const char *ip = st->conn ? net_wifi_ip() : (st->ap ? net_wifi_ap_ip() : "");
    if (ip) {
        strncpy(st->ip, ip, sizeof(st->ip) - 1);
    } else {
        st->ip[0] = 0;
    }
    st->ip[sizeof(st->ip) - 1] = 0;

    st->running = net_httpd_running();

    net_httpd_prog_t p;
    net_httpd_prog(&p);
    st->files   = p.files;
    st->percent = p.percent;
}

static bool state_changed(const recv_state_t *a, const recv_state_t *b)
{
    return a->conn   != b->conn   ||
           a->ap     != b->ap     ||
           a->running != b->running ||
           a->files  != b->files  ||
           a->percent != b->percent ||
           strcmp(a->ip, b->ip) != 0;
}

static int recv_hit(int x, int y, void *ctx){
    (void)ctx;
    if (y >= FOOT_Y && y < FOOT_Y + FOOT_H && x >= 82 && x < 82 + 76) {
        return RH_BACK;
    }
    return RH_NONE;
}

void net_recv_run(void)
{

    int pressed = RH_NONE;

    if (net_wifi_connected() || net_wifi_ap_on()) net_httpd_start();

    read_state(&s_shown);
    recv_draw(pressed, &s_shown);
    ESP_LOGI(TAG, "文件接收页就绪（服务%s，IP %s）",
             s_shown.running ? "已启动" : "未启动",
             s_shown.conn ? s_shown.ip : "无");

    int64_t last_poll = 0;

    for (;;) {

        const int64_t t = now_ms();
        if (t - last_poll >= POLL_MS) {
            last_poll = t;

            recv_state_t cur;
            read_state(&cur);

            if ((cur.conn || cur.ap) && !cur.running) {
                net_httpd_start();
                read_state(&cur);
            }

            if (state_changed(&cur, &s_shown)) {
                s_shown = cur;
                recv_draw(pressed, &s_shown);
            }
        }

        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(recv_hit, NULL);
            if (h != pressed) {
                pressed = h;
                recv_draw(pressed, &s_shown);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(recv_hit, NULL);
        pressed = RH_NONE;

        if (h == RH_BACK) return;
        recv_draw(pressed, &s_shown);
    }
}

#if NET_RECV_SELFTEST

void net_recv_selftest(void)
{
    ESP_LOGI(TAG, "=== 文件接收自检 ===");

    if (net_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 起不来");
        return;
    }

    char ssid[33], pass[65];
    if (!net_wifi_load_saved(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGW(TAG, "NVS 里没有 WiFi 凭据 —— 先在板子上连一次 WLAN，再重跑自检");
        return;
    }

    ESP_LOGI(TAG, "连接 %s …", ssid);
    net_wifi_connect(ssid, pass, false);

    for (int i = 0; i < 120 && !net_wifi_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!net_wifi_connected()) {
        ESP_LOGE(TAG, "连不上 %s（等了 24 秒）", ssid);
        return;
    }

    ESP_LOGI(TAG, "已连上，IP = %s", net_wifi_ip());
    if (net_httpd_start() == ESP_OK) {
        ESP_LOGI(TAG, "HTTP 服务就绪 —— PC 上可试: curl http://%s/", net_wifi_ip());
    }

    {
        const char *pa = "/sdcard/images/_fat_probe_ascii.bin";
        FILE *fa = fopen(pa, "wb");
        if (fa) { fclose(fa); unlink(pa); ESP_LOGI(TAG, "FATFS 写 ASCII 文件名：可以"); }
        else    { ESP_LOGE(TAG, "FATFS 写 ASCII 文件名：不行 errno=%d", errno); }

        const char *pz = "/sdcard/images/\xe6\xb5\x8b\xe8\xaf\x95.bin";
        FILE *fz = fopen(pz, "wb");
        if (fz) { fclose(fz); unlink(pz); ESP_LOGI(TAG, "FATFS 写中文文件名：可以"); }
        else    { ESP_LOGE(TAG, "FATFS 写中文文件名：不行 errno=%d", errno); }
    }
    net_wifi_mem("自检后");
    ESP_LOGI(TAG, "=== 自检结束 ===");
}
#endif
