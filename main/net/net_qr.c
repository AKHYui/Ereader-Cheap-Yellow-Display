#include "net_qr.h"

#include "net_wifi.h"
#include "nui_qr.h"
#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = PV_TAG;

#define QR_WHITE   0xFFFF
#define QR_BLACK   0x0000

#define QR_MOD       6
#define QR_QUIET     4
#define QR_TOP       8
#define HINT_Y     238
#define HINT_H      32
#define FOOT_Y     272
#define FOOT_H      48

#define BTN_W       76
#define BTN_GAP      8

enum { QH_NONE = 0, QH_SWAP = 1, QH_BACK = 2 };

static char s_text[2][128];
static bool s_ready[2];

static int qr_btn_x(int which)
{
    const int total = BTN_W * 2 + BTN_GAP;
    return (PV_SCR_W - total) / 2 + (which == QH_BACK ? BTN_W + BTN_GAP : 0);
}

static int cur_qr_size(int mode)
{
    if (!s_ready[mode]) return 0;
    return (nui_qr_size() > 0) ? nui_qr_size() : 0;
}

static void qr_draw(int mode, int pressed)
{
    const int size = cur_qr_size(mode);
    const int total = (size > 0) ? (size + QR_QUIET * 2) * QR_MOD : 0;
    const int x0 = (PV_SCR_W - total) / 2;

    pv_disp_page_begin(QR_WHITE);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        if (size > 0 && sy >= QR_TOP && sy < QR_TOP + total) {
            const int mr = (sy - QR_TOP) / QR_MOD - QR_QUIET;
            if (mr >= 0 && mr < size) {
                for (int c = 0; c < size; c++) {
                    if (!nui_qr_dark(mr, c)) continue;
                    const int x = x0 + (c + QR_QUIET) * QR_MOD;
                    nui_hspan(row, x, x + QR_MOD, QR_BLACK);
                }
            }
        }

        nui_text_mid_center(row, sy, PV_SCR_W / 2, HINT_Y, HINT_H,
                            size > 0 ? (mode == 0 ? "扫码连热点" : "扫码开网页")
                                     : "热点未开启",
                            size > 0 ? QR_BLACK : NUI_ERR, QR_WHITE);

        {
            static const char *const LBL[2] = { "切换", "返回" };
            for (int i = 0; i < 2; i++) {
                const int which = i + 1;
                const int bx = qr_btn_x(which);
                const bool on = (pressed == which);
                nui_frame_row(row, sy, bx, FOOT_Y, BTN_W, FOOT_H,
                              on ? NUI_PRESS : QR_WHITE,
                              QR_BLACK, 2);
                nui_text_mid_center(row, sy, bx + BTN_W / 2, FOOT_Y, FOOT_H,
                                    LBL[i], QR_BLACK, on ? NUI_PRESS : QR_WHITE);
            }
        }
    }

    pv_disp_page_end();
}

static int qr_hit(int x, int y, void *ctx)
{
    (void)ctx;
    if (y < FOOT_Y || y >= FOOT_Y + FOOT_H) return QH_NONE;

    for (int w = QH_SWAP; w <= QH_BACK; w++) {
        const int bx = qr_btn_x(w);
        if (x >= bx && x < bx + BTN_W) return w;
    }
    return QH_NONE;
}

static int build_texts(void)
{
    const char *ssid = net_wifi_ap_ssid();
    const char *ip   = net_wifi_ap_ip();

    s_ready[0] = s_ready[1] = false;

    if (!net_wifi_ap_on() || !ssid[0]) {
        ESP_LOGW(TAG, "扫码页：热点没开，没有可显示的内容");
        return 0;
    }

    const size_t plen = strlen(NET_WIFI_AP_PASS);
    if (plen >= 8) {
        snprintf(s_text[0], sizeof(s_text[0]),
                 "WIFI:T:WPA;S:%s;P:%s;;", ssid, NET_WIFI_AP_PASS);
    } else {
        snprintf(s_text[0], sizeof(s_text[0]), "WIFI:T:nopass;S:%s;;", ssid);
    }
    s_ready[0] = true;

    if (ip[0]) {
        snprintf(s_text[1], sizeof(s_text[1]), "http://%s/", ip);
        s_ready[1] = true;
    }
    return (int)s_ready[0] + (int)s_ready[1];
}

static bool encode_for(int mode)
{
    s_ready[mode] = (s_text[mode][0] != 0);
    if (!s_ready[mode]) return false;
    return nui_qr_encode(s_text[mode]) > 0;
}

void net_qr_run(void)
{
    const int n = build_texts();
    int mode = 0;
    int pressed = QH_NONE;

    if (n > 0) {

        if (!encode_for(0)) {
            mode = 1;
            encode_for(1);
        }
        ESP_LOGI(TAG, "扫码页就绪（内容「%s」-> %d 模块，掩码 %d）",
                 s_text[mode], nui_qr_size(), nui_qr_mask());
    }

    qr_draw(mode, pressed);

    for (;;) {
        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(qr_hit, NULL);
            if (h != pressed) {
                pressed = h;
                qr_draw(mode, pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(qr_hit, NULL);
        pressed = QH_NONE;

        if (h == QH_BACK) return;

        if (h == QH_SWAP) {
            if (n >= 2) {
                mode = 1 - mode;
                if (!encode_for(mode)) mode = 1 - mode;
                ESP_LOGI(TAG, "切换到「%s」", s_text[mode]);
            } else {
                ESP_LOGW(TAG, "只有一个可用内容，切不了");
            }
            qr_draw(mode, pressed);
            continue;
        }

        qr_draw(mode, pressed);
    }
}
