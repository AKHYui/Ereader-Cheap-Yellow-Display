#include "net_wlan.h"

#include "net_httpd.h"
#include "net_wifi.h"
#include "nui_ascii.h"
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
#include <string.h>

static const char *TAG = "net_wlan";

#define BAR_H        44
#define LIST_Y       48
#define LIST_ROW_H   36
#define LIST_ROWS    6
#define FOOT_Y      272
#define FOOT_H       48

#define PB_BAR_H     40
#define PB_BOX_Y     62
#define PB_BOX_H     34
#define KB_Y        100
#define KB_KEY_W     40
#define KB_KEY_H     44
#define KB_ROWS      5
#define KB_COLS      6

#define CONN_TIMEOUT_MS  15000

enum {
    H_NONE = 0,
    H_UP, H_DN, H_SCAN, H_BACK,
    H_CANCEL, H_OK,
    H_RETRY,
    H_AP  = 100,
    H_KEY = 200,
};

#define K_SW  '\x01'
#define K_BS  '\x02'

static const char *KB_PAGE[3] = {
    "abcdef" "ghijkl" "mnopqr" "stuvwx" "yz.-" "\x01\x02",
    "123456" "7890.@" "_#$%&*" "+=!?/\\" ";,'\"" "\x01\x02",
    "ABCDEF" "GHIJKL" "MNOPQR" "STUVWX" "YZ.-" "\x01\x02",
};

static const char *KB_SW_LABEL[3] = { "123", "ABC", "abc" };

typedef enum { V_LIST = 0, V_PASS, V_BUSY } view_t;

static view_t   s_view;
static int      s_press;

static net_ap_t s_aps[NET_WIFI_MAX_AP];
static int      s_nap;
static int      s_scroll;
static int      s_sel = -1;

static char     s_ssid[NET_WIFI_SSID_LEN];
static bool     s_open_net;
static char     s_pass[NET_WIFI_PASS_LEN];
static int      s_pass_len;
static int      s_kb_page;

static bool     s_busy;
static int64_t  s_busy_t0;
static bool     s_fail;

static int rssi_level(int8_t rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    return 1;
}

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void button_row(uint16_t *row, int sy, int x, int y, int w, int h,
                       const char *label, bool pressed)
{
    const uint16_t bg = pressed ? NUI_PRESS : NUI_PANEL;
    nui_frame_row(row, sy, x, y, w, h, bg, NUI_EDGE, 2);
    if (label && label[0]) {
        nui_text_center(row, sy, x + w / 2, y + (h - nui_text_h()) / 2,
                        label, NUI_FG, bg);
    }
}

static void foot_rect(int which, int *x, int *y, int *w, int *h)
{
    switch (which) {
    case H_UP:   *x =   4; *w = 34; break;
    case H_DN:   *x =  42; *w = 34; break;
    case H_SCAN: *x =  80; *w = 76; break;
    default:     *x = 160; *w = 76; break;
    }
    *y = FOOT_Y;
    *h = FOOT_H;
}

static int ap_rect(int row, int *x, int *y, int *w, int *h)
{
    const int idx = s_scroll + row;
    if (row < 0 || row >= LIST_ROWS || idx >= s_nap) return 0;
    *x = 6;
    *y = LIST_Y + row * LIST_ROW_H;
    *w = PV_SCR_W - 12;
    *h = LIST_ROW_H - 2;
    return 1;
}

static void list_draw_row(uint16_t *row, int sy)
{

    const bool  conn = net_wifi_connected();
    const char *st   = conn ? "已连接" : "未连接";

    nui_text_mid(row, sy, 12, 2, 20, "WLAN", NUI_FG, NUI_BG);
    nui_text_mid(row, sy, PV_SCR_W - 12 - nui_text_w(st), 2, 20, st,
                 conn ? NUI_OK : NUI_DIM, NUI_BG);

    if (conn) {

        const char *ip = net_wifi_ip();
        if (ip && ip[0]) nui_text_mid(row, sy, 12, 24, 20, ip, NUI_DIM, NUI_BG);
    }
    if (sy >= BAR_H && sy < BAR_H + 2) nui_hspan(row, 6, PV_SCR_W - 6, NUI_EDGE);

    if (s_nap <= 0) {

        nui_text_mid_center(row, sy, PV_SCR_W / 2, LIST_Y + 60, 40,
                            "未找到", NUI_DIM, NUI_BG);
    }

    for (int i = 0; i < LIST_ROWS; i++) {
        int x, y, w, h;
        if (!ap_rect(i, &x, &y, &w, &h)) continue;
        const int idx = s_scroll + i;
        if (sy < y || sy >= y + h) continue;

        const bool hit = (s_press == H_AP + idx);
        const bool cur = (s_sel == idx);
        const uint16_t bg = hit ? NUI_PRESS : (cur ? NUI_EDGE : NUI_PANEL);

        nui_frame_row(row, sy, x, y, w, h, bg, NUI_EDGE, 1);

        nui_sig_row(row, sy, x + 8, y + (h - 14) / 2, rssi_level(s_aps[idx].rssi),
                    NUI_FG);

        nui_text_at(row, sy, x + 28, y + (h - nui_text_h()) / 2,
                    s_aps[idx].ssid, NUI_FG, bg);
        if (s_aps[idx].secure) {
            nui_lock_row(row, sy, x + w - 20, y + (h - 14) / 2, NUI_DIM);
        }
    }

    button_row(row, sy, 4, FOOT_Y, 34, FOOT_H, NULL, s_press == H_UP);
    nui_tri_row(row, sy, 4 + 17, FOOT_Y + FOOT_H / 2, 12, 0, NUI_FG);

    button_row(row, sy, 42, FOOT_Y, 34, FOOT_H, NULL, s_press == H_DN);
    nui_tri_row(row, sy, 42 + 17, FOOT_Y + FOOT_H / 2, 12, 1, NUI_FG);

    button_row(row, sy, 80, FOOT_Y, 76, FOOT_H, "扫描", s_press == H_SCAN);
    button_row(row, sy, 160, FOOT_Y, 76, FOOT_H, "返回", s_press == H_BACK);
}

static void key_rect(int r, int c, int *x, int *y)
{
    *x = c * KB_KEY_W;
    *y = KB_Y + r * KB_KEY_H;
}

static void pass_draw_row(uint16_t *row, int sy)
{
    const char *page = KB_PAGE[s_kb_page];

    const uint16_t bg_l = (s_press == H_CANCEL) ? NUI_PRESS : NUI_PANEL;
    const uint16_t bg_r = (s_press == H_OK)     ? NUI_PRESS : NUI_PANEL;

    nui_frame_row(row, sy, 0, 0, PB_BAR_H, PB_BAR_H, bg_l, NUI_EDGE, 2);
    nui_text_mid_center(row, sy, PB_BAR_H / 2, 0, PB_BAR_H, "X", NUI_FG, bg_l);

    nui_text_mid_center(row, sy, PV_SCR_W / 2, 0, PB_BAR_H, "请输入密码", NUI_FG, NUI_BG);

    nui_frame_row(row, sy, PV_SCR_W - PB_BAR_H, 0, PB_BAR_H, PB_BAR_H, bg_r, NUI_EDGE, 2);
    nui_text_mid_center(row, sy, PV_SCR_W - PB_BAR_H / 2, 0, PB_BAR_H, "OK", NUI_FG, bg_r);

    if (sy >= PB_BAR_H && sy < PB_BAR_H + 2) nui_hspan(row, 0, PV_SCR_W, NUI_EDGE);

    nui_text_mid_center(row, sy, PV_SCR_W / 2, PB_BAR_H + 2,
                        PB_BOX_Y - PB_BAR_H - 2, s_ssid, NUI_DIM, NUI_BG);

    nui_frame_row(row, sy, 8, PB_BOX_Y, PV_SCR_W - 16, PB_BOX_H, NUI_BG, NUI_EDGE, 2);

    const int max_ch = (PV_SCR_W - 16 - 20) / nui_ascii_w();
    const int from   = (s_pass_len > max_ch) ? (s_pass_len - max_ch) : 0;
    char buf[NET_WIFI_PASS_LEN];
    int  n = 0;
    for (int i = from; i < s_pass_len && n < (int)sizeof(buf) - 1; i++) {
        buf[n++] = s_pass[i];
    }
    buf[n] = 0;

    nui_text_mid(row, sy, 16, PB_BOX_Y, PB_BOX_H, buf, NUI_FG, NUI_BG);

    const int cur_x = 16 + n * nui_ascii_w() + 1;
    const int cur_y = PB_BOX_Y + (PB_BOX_H - 14) / 2;
    if (sy >= cur_y && sy < cur_y + 14) nui_hspan(row, cur_x, cur_x + 6, NUI_FG);

    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            int x, y;
            key_rect(r, c, &x, &y);
            if (sy < y || sy >= y + KB_KEY_H) continue;

            const char k = page[r * KB_COLS + c];
            if (k == 0) continue;

            const bool hit = (s_press == H_KEY + r * KB_COLS + c);
            const uint16_t bg = hit ? NUI_PRESS : NUI_PANEL;
            nui_frame_row(row, sy, x + 1, y + 1, KB_KEY_W - 2, KB_KEY_H - 2,
                          bg, NUI_EDGE, 1);

            char lbl[2] = { k, 0 };
            const char *text = lbl;
            if (k == K_SW)      text = KB_SW_LABEL[s_kb_page];
            else if (k == K_BS) text = "<-";

            nui_text_center(row, sy, x + KB_KEY_W / 2,
                            y + (KB_KEY_H - nui_text_h()) / 2, text, NUI_FG, bg);
        }
    }
}

static void busy_draw_row(uint16_t *row, int sy)
{
    nui_text_mid(row, sy, 12, 2, 20, "WLAN", NUI_FG, NUI_BG);
    if (sy >= BAR_H && sy < BAR_H + 2) nui_hspan(row, 6, PV_SCR_W - 6, NUI_EDGE);

    const char *msg = s_fail ? "连接失败" : "连接中";
    const int    y   = 110;

    nui_text_mid_center(row, sy, PV_SCR_W / 2, y, 40, msg,
                        s_fail ? NUI_ERR : NUI_FG, NUI_BG);
    nui_text_mid_center(row, sy, PV_SCR_W / 2, y + 40, 20, s_ssid, NUI_DIM, NUI_BG);

    if (s_fail) {
        button_row(row, sy, 34, FOOT_Y, 76, FOOT_H, "重试", s_press == H_RETRY);
        button_row(row, sy, 130, FOOT_Y, 76, FOOT_H, "返回", s_press == H_BACK);
    } else {

        button_row(row, sy, 160, FOOT_Y, 76, FOOT_H, "返回", s_press == H_BACK);
    }
}

static void wlan_draw(void)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;
        switch (s_view) {
        case V_PASS: pass_draw_row(row, sy); break;
        case V_BUSY: busy_draw_row(row, sy); break;
        default:     list_draw_row(row, sy); break;
        }
    }

    pv_disp_page_end();
}

static int wlan_hit(int x, int y, void *ctx)
{
    (void)ctx;

    if (s_view == V_PASS) {
        if (x < PB_BAR_H && y < PB_BAR_H) return H_CANCEL;
        if (x >= PV_SCR_W - PB_BAR_H && y < PB_BAR_H) return H_OK;

        if (y >= KB_Y && y < KB_Y + KB_ROWS * KB_KEY_H &&
            x >= 0 && x < KB_COLS * KB_KEY_W) {
            const int c = x / KB_KEY_W;
            const int r = (y - KB_Y) / KB_KEY_H;
            return H_KEY + r * KB_COLS + c;
        }
        return H_NONE;
    }

    if (s_view == V_BUSY) {
        if (s_fail) {
            if (y >= FOOT_Y && y < FOOT_Y + FOOT_H) {
                if (x >= 34 && x < 110)  return H_RETRY;
                if (x >= 130 && x < 206) return H_BACK;
            }
        } else {
            if (y >= FOOT_Y && y < FOOT_Y + FOOT_H && x >= 160 && x < 236) {
                return H_BACK;
            }
        }
        return H_NONE;
    }

    for (int i = 0; i < LIST_ROWS; i++) {
        int ax, ay, aw, ah;
        if (!ap_rect(i, &ax, &ay, &aw, &ah)) continue;
        if (x >= ax && x < ax + aw && y >= ay && y < ay + ah) {
            return H_AP + s_scroll + i;
        }
    }

    if (y >= FOOT_Y && y < FOOT_Y + FOOT_H) {
        int bx, by, bw, bh;
        for (int k = H_UP; k <= H_BACK; k++) {
            foot_rect(k, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw) return k;
        }
    }
    return H_NONE;
}

static void do_scan(void)
{

    s_view = V_LIST;
    s_nap  = 0;
    s_scroll = 0;
    s_press = H_NONE;
    wlan_draw();

    pv_touch_evt_t ev;
    while (pv_touch_take(&ev)) { }

    s_nap = net_wifi_scan(s_aps, NET_WIFI_MAX_AP);
    if (s_nap < 0) s_nap = 0;

    ESP_LOGI(TAG, "列表 %d 个 AP，滚动位置 %d", s_nap, s_scroll);
    wlan_draw();
}

static void start_connect(void)
{

    if (!s_ssid[0]) return;

    if (net_wifi_connect(s_ssid, s_open_net ? "" : s_pass, true) != ESP_OK) {
        s_view = V_BUSY;
        s_fail = true;
        s_busy = false;
        wlan_draw();
        return;
    }
    s_view    = V_BUSY;
    s_busy    = true;
    s_fail    = false;
    s_busy_t0 = now_ms();
    wlan_draw();
}

static void pick_ap(int idx)
{
    if (idx < 0 || idx >= s_nap) return;

    s_sel = idx;
    strncpy(s_ssid, s_aps[idx].ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = 0;
    s_open_net = !s_aps[idx].secure;

    char sv_ssid[NET_WIFI_SSID_LEN], sv_pass[NET_WIFI_PASS_LEN];
    if (net_wifi_load_saved(sv_ssid, sizeof(sv_ssid), sv_pass, sizeof(sv_pass)) &&
        strcmp(sv_ssid, s_ssid) == 0) {
        strncpy(s_pass, sv_pass, sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = 0;
        s_pass_len = (int)strlen(s_pass);
    } else {
        s_pass[0]  = 0;
        s_pass_len = 0;
    }
    s_kb_page = 0;

    if (!s_aps[idx].secure) {
        ESP_LOGI(TAG, "开放网络，直接连接：%s", s_ssid);
        start_connect();
        return;
    }

    s_view = V_PASS;
    wlan_draw();
}

static void key_press(char k)
{
    if (k == K_BS) {
        if (s_pass_len > 0) s_pass[--s_pass_len] = 0;
        return;
    }
    if (k == K_SW) {
        s_kb_page = (s_kb_page + 1) % 3;
        return;
    }
    if (s_pass_len < (int)sizeof(s_pass) - 1) {
        s_pass[s_pass_len++] = k;
        s_pass[s_pass_len]   = 0;
    }
}

void net_wlan_run(void)
{
    ESP_LOGI(TAG, "=== WLAN 页 ===");

    s_view     = V_LIST;
    s_press    = H_NONE;
    s_scroll   = 0;
    s_nap      = 0;
    s_sel      = -1;
    s_busy     = false;
    s_fail     = false;
    s_pass_len = 0;
    s_kb_page  = 0;
    s_pass[0]  = 0;
    s_ssid[0]  = 0;

    if (net_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 启动失败，本页不可用");
        pv_disp_page_begin(NUI_BG);
        for (int sy = 0; sy < PV_SCR_H; sy++) {
            uint16_t *row = pv_disp_page_row(sy);
            if (!row) continue;
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 140, 40, "连接失败",
                                NUI_ERR, NUI_BG);
            if (sy >= FOOT_Y && sy < FOOT_Y + FOOT_H) {
                button_row(row, sy, 160, FOOT_Y, 76, FOOT_H, "返回", false);
            }
        }
        pv_disp_page_end();

        for (;;) {
            pv_touch_evt_t ev;
            if (!pv_touch_take(&ev)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            if (!ev.down) {
                const int h = pv_touch_hit(wlan_hit, NULL);
                if (h == H_BACK || h == H_RETRY) return;
            }
        }
    }

    do_scan();

    {
        char sv_ssid[NET_WIFI_SSID_LEN], sv_pass[NET_WIFI_PASS_LEN];
        if (!net_wifi_connected() &&
            net_wifi_load_saved(sv_ssid, sizeof(sv_ssid), sv_pass, sizeof(sv_pass))) {
            strncpy(s_ssid, sv_ssid, sizeof(s_ssid) - 1);
            s_ssid[sizeof(s_ssid) - 1] = 0;
            strncpy(s_pass, sv_pass, sizeof(s_pass) - 1);
            s_pass[sizeof(s_pass) - 1] = 0;
            s_pass_len = (int)strlen(s_pass);

            bool found = false;
            for (int i = 0; i < s_nap; i++) {
                if (strcmp(s_aps[i].ssid, s_ssid) == 0) {
                    s_sel      = i;
                    s_open_net = !s_aps[i].secure;
                    found      = true;
                    break;
                }
            }
            if (!found) s_open_net = (s_pass_len == 0);

            ESP_LOGI(TAG, "有保存的凭据，自动连接 %s", s_ssid);
            start_connect();
        }
    }

    for (;;) {
        pv_touch_evt_t ev;

        if (s_busy) {
            if (net_wifi_connected()) {
                s_busy = false;
                s_fail = false;
                s_view = V_LIST;
                ESP_LOGI(TAG, "连接成功，IP %s", net_wifi_ip());

                net_httpd_start();
                wlan_draw();
            } else if (now_ms() - s_busy_t0 > CONN_TIMEOUT_MS) {
                s_busy = false;
                s_fail = true;
                ESP_LOGW(TAG, "连接超时（%d ms）", CONN_TIMEOUT_MS);
                wlan_draw();
            }
        }

        if (pv_touch_is_down()) {
            const int now = pv_touch_hit(wlan_hit, NULL);
            if (now != s_press) {
                s_press = now;
                wlan_draw();
            }
        }

        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ev.down) continue;

        const int h = s_press;
        s_press = H_NONE;

        if (h == H_NONE) {
            wlan_draw();
            continue;
        }

        switch (h) {
        case H_BACK:
            if (s_view == V_PASS)       { s_view = V_LIST; wlan_draw(); }
            else                        { return; }
            break;

        case H_CANCEL:
            s_view = V_LIST;
            wlan_draw();
            break;

        case H_OK:

            if (s_pass_len == 0) {
                ESP_LOGW(TAG, "密码为空，不发起连接（加密网络必须有密码）");
                wlan_draw();
                break;
            }
            start_connect();
            break;

        case H_RETRY:
            start_connect();
            break;

        case H_SCAN:
            do_scan();
            break;

        case H_UP:
            if (s_scroll > 0) { s_scroll--; wlan_draw(); }
            else wlan_draw();
            break;

        case H_DN:
            if (s_scroll + LIST_ROWS < s_nap) { s_scroll++; wlan_draw(); }
            else wlan_draw();
            break;

        default:
            if (h >= H_KEY && h < H_KEY + KB_ROWS * KB_COLS) {
                const char k = KB_PAGE[s_kb_page][h - H_KEY];
                key_press(k);
                wlan_draw();
            } else if (h >= H_AP) {
                pick_ap(h - H_AP);
            } else {
                wlan_draw();
            }
            break;
        }
    }
}
