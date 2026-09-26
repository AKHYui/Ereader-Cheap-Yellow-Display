#include "net_menu.h"

#include "net_ap.h"
#include "net_recv.h"
#include "net_wifi.h"
#include "net_wlan.h"
#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static const char *TAG = PV_TAG;

#define N_ITEM 4

enum { MH_NONE = 0, MH_WLAN = 1, MH_AP = 2, MH_RECV = 3, MH_BACK = 4 };

_Static_assert(MH_BACK - MH_WLAN == N_ITEM - 1, "MH_* 必须与条目顺序一一对应且连续");

static void net_menu_draw(int pressed)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "网络");

        {
            const uint16_t bg = nui_item_row(row, sy, 0, pressed == 0);
            const int      y  = nui_item_y(0);

            nui_text_mid(row, sy, NUI_ITEM_X + 18, y, NUI_ITEM_H, "WLAN", NUI_FG, bg);

            const bool  ok = net_wifi_connected();
            const char *st = ok ? "已连接" : "未连接";
            nui_text_mid(row, sy, NUI_ITEM_X + NUI_ITEM_W - 34 - nui_text_w(st),
                         y, NUI_ITEM_H, st, ok ? NUI_OK : NUI_DIM, bg);
            nui_text_mid(row, sy, NUI_ITEM_X + NUI_ITEM_W - 22, y, NUI_ITEM_H,
                         ">", NUI_FG, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 1, pressed == 1);
            const int      y  = nui_item_y(1);

            nui_text_mid(row, sy, NUI_ITEM_X + 18, y, NUI_ITEM_H, "AP模式", NUI_FG, bg);
            nui_text_mid(row, sy, NUI_ITEM_X + NUI_ITEM_W - 22, y, NUI_ITEM_H,
                         ">", NUI_FG, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 2, pressed == 2);
            const int      y  = nui_item_y(2);

            nui_text_mid(row, sy, NUI_ITEM_X + 18, y, NUI_ITEM_H, "文件接收", NUI_FG, bg);
            nui_text_mid(row, sy, NUI_ITEM_X + NUI_ITEM_W - 22, y, NUI_ITEM_H,
                         ">", NUI_FG, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 3, pressed == 3);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(3), NUI_ITEM_H,
                         "返回", NUI_FG, bg);
        }
    }

    pv_disp_page_end();
}

static int net_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < N_ITEM; i++) {
        const int iy = nui_item_y(i);
        if (x >= NUI_ITEM_X && x < NUI_ITEM_X + NUI_ITEM_W &&
            y >= iy && y < iy + NUI_ITEM_H) {
            return MH_WLAN + i;
        }
    }
    return MH_NONE;
}

void net_menu_run(void)
{
    int pressed = -1;
    net_menu_draw(pressed);
    ESP_LOGI(TAG, "网络菜单就绪（WLAN / AP模式 / 文件接收 / 返回）");

    for (;;) {

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
            return;
        } else {
            net_menu_draw(pressed);
        }
    }
}
