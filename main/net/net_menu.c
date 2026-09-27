#include "net_menu.h"

#include "net_ap.h"
#include "net_recv.h"
#include "net_remote.h"
#include "net_wifi.h"
#include "net_wlan.h"
#include "nui_ui.h"
#include "nui_sleep.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = PV_TAG;

#define N_ITEM 5

static void item_arrow(uint16_t *row, int sy, int i, bool pressed, const char *label)
{
    const uint16_t bg = nui_crow(row, sy, i, pressed);
    nui_text_mid(row, sy, NUI_CX + 18, nui_cy(i), NUI_CH, label, NUI_FG, bg);
    nui_text_mid(row, sy, NUI_CX + NUI_CW - 22, nui_cy(i), NUI_CH, ">", NUI_FG, bg);
}

enum { MH_NONE = 0, MH_WLAN = 1, MH_AP = 2, MH_RECV = 3, MH_REMOTE = 4, MH_BACK = 5 };

_Static_assert(MH_BACK - MH_WLAN == N_ITEM - 1, "MH_* 必须与条目顺序一一对应且连续");

static void net_menu_draw(int pressed)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "网络");

        {
            const uint16_t bg = nui_crow(row, sy, 0, pressed == 0);
            const int      y  = nui_cy(0);

            nui_text_mid(row, sy, NUI_CX + 18, y, NUI_CH, "WLAN", NUI_FG, bg);

            const bool  ok = net_wifi_connected();
            const char *st = ok ? "已连接" : "未连接";
            nui_text_mid(row, sy, NUI_CX + NUI_CW - 34 - nui_text_w(st),
                         y, NUI_CH, st, ok ? NUI_OK : NUI_DIM, bg);
            nui_text_mid(row, sy, NUI_CX + NUI_CW - 22, y, NUI_CH,
                         ">", NUI_FG, bg);
        }

        item_arrow(row, sy, 1, pressed == 1, "AP模式");
        item_arrow(row, sy, 2, pressed == 2, "文件接收");
        item_arrow(row, sy, 3, pressed == 3, "网页遥控");

        nui_ctext(row, sy, 4, "返回", NUI_FG, nui_crow(row, sy, 4, pressed == 4));
    }

    pv_disp_page_end();
}

static int net_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < N_ITEM; i++) {
        const int iy = nui_cy(i);
        if (x >= NUI_CX && x < NUI_CX + NUI_CW &&
            y >= iy && y < iy + NUI_CH) {
            return MH_WLAN + i;
        }
    }
    return MH_NONE;
}

void net_menu_run(void)
{
    int pressed = -1;
    net_menu_draw(pressed);
    ESP_LOGI(TAG, "网络菜单就绪（WLAN / AP模式 / 文件接收 / 网页遥控 / 返回）");

    for (;;) {

        if (nui_sleep_poll()) {
            pressed = -1;
            net_menu_draw(pressed);
            continue;
        }

        if (pv_touch_is_down()) {
            const int h   = pv_touch_hit(net_hit, NULL);
            const int idx = (h == MH_NONE) ? -1 : (h - MH_WLAN);
            if (idx != pressed) {
                pressed = idx;
                net_menu_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ev.down) continue;

        const int h   = pv_touch_hit(net_hit, NULL);
        const int idx = h - MH_WLAN;
        pressed = -1;

        if (idx == 0) {
            net_wlan_run();
            net_menu_draw(pressed);
        } else if (idx == 1) {
            net_ap_run();
            net_menu_draw(pressed);
        } else if (idx == 2) {
            net_recv_run();
            net_menu_draw(pressed);
        } else if (idx == 3) {
            net_remote_run();
            net_menu_draw(pressed);
        } else if (idx == 4) {
            return;
        } else {
            net_menu_draw(pressed);
        }
    }
}
