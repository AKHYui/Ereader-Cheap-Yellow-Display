#include "nui_menu.h"
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

#define TILE_W      102
#define TILE_H      142
#define GRID_MARGIN  12
#define GRID_GAP     12

#define ICON_W       46
#define ICON_H       36
#define ICON_Y       25
#define LABEL_Y      85

static const char *LBL[4] = { "阅读", "图片", "网络", "设置" };

static int tile_x(int t) { return GRID_MARGIN + (t & 1) * (TILE_W + GRID_GAP); }
static int tile_y(int t) { return GRID_MARGIN + (t >> 1) * (TILE_H + GRID_GAP); }

static int isqrt_small(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void icon_read_row(uint16_t *row, int sy, int bx, int by, uint16_t c)
{
    if (sy < by || sy >= by + ICON_H) return;
    const int ly = sy - by;

    if (ly < 2 || ly >= ICON_H - 2) {
        nui_hspan(row, bx, bx + ICON_W, c);
    } else {
        nui_hspan(row, bx, bx + 2, c);
        nui_hspan(row, bx + ICON_W - 2, bx + ICON_W, c);
        nui_hspan(row, bx + ICON_W / 2 - 1, bx + ICON_W / 2 + 1, c);
    }
}

static void icon_image_row(uint16_t *row, int sy, int bx, int by, uint16_t c)
{
    if (sy < by || sy >= by + ICON_H) return;
    const int ly = sy - by;

    if (ly >= ICON_H - 18 && ly < ICON_H - 2) {
        const int k = ly - (ICON_H - 18);
        nui_hspan(row, bx + 5, bx + 5 + (k + 1) * 2 + 2, c);
    }

    {
        const int dy = ly - 11;
        if (dy >= -4 && dy <= 4) {
            const int hw = isqrt_small(16 - dy * dy);
            nui_hspan(row, bx + 13 - hw, bx + 13 + hw + 1, c);
        }
    }

    if (ly < 2 || ly >= ICON_H - 2) {
        nui_hspan(row, bx, bx + ICON_W, c);
    } else {
        nui_hspan(row, bx, bx + 2, c);
        nui_hspan(row, bx + ICON_W - 2, bx + ICON_W, c);
    }
}

static void icon_net_row(uint16_t *row, int sy, int bx, int by, uint16_t c)
{

    static const int bars[4] = { 10, 18, 26, 34 };
    const int bottom = by + ICON_H;
    if (sy < by || sy >= bottom) return;

    for (int i = 0; i < 4; i++) {
        const int x = bx + 3 + i * 11;
        if (sy >= bottom - bars[i]) nui_hspan(row, x, x + 8, c);
    }
}

static void icon_set_row(uint16_t *row, int sy, int bx, int by, uint16_t c)
{
    if (sy < by || sy >= by + ICON_H) return;
    const int y1 = by + 11, y2 = by + 29;

    if (sy >= y1 - 1 && sy <= y1 + 1) nui_hspan(row, bx + 3, bx + ICON_W - 3, c);
    if (sy >= y1 - 7 && sy <= y1 + 7) nui_hspan(row, bx + 12, bx + 24, c);
    if (sy >= y2 - 1 && sy <= y2 + 1) nui_hspan(row, bx + 3, bx + ICON_W - 3, c);
    if (sy >= y2 - 7 && sy <= y2 + 7) nui_hspan(row, bx + 32, bx + 44, c);
}

static void tile_row(uint16_t *row, int sy, int t, bool pressed)
{
    const int tx = tile_x(t), ty = tile_y(t);
    if (sy < ty || sy >= ty + TILE_H) return;

    const int ly = sy - ty;

    const uint16_t bg = pressed ? NUI_PRESS : NUI_PANEL;
    nui_hspan(row, tx, tx + TILE_W, bg);

    if (ly < 2 || ly >= TILE_H - 2) {
        nui_hspan(row, tx, tx + TILE_W, NUI_EDGE);
    } else {
        nui_hspan(row, tx, tx + 2, NUI_EDGE);
        nui_hspan(row, tx + TILE_W - 2, tx + TILE_W, NUI_EDGE);
    }

    const int bx = tx + (TILE_W - ICON_W) / 2;
    const int by = ty + ICON_Y;
    switch (t) {
    case 0: icon_read_row(row, sy, bx, by, NUI_FG);  break;
    case 1: icon_image_row(row, sy, bx, by, NUI_FG); break;
    case 2: icon_net_row(row, sy, bx, by, NUI_FG);   break;
    default: icon_set_row(row, sy, bx, by, NUI_FG);  break;
    }

    nui_text_center(row, sy, tx + TILE_W / 2, ty + LABEL_Y, LBL[t], NUI_FG, bg);
}

static void menu_draw(int pressed)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;
        for (int t = 0; t < 4; t++) {
            tile_row(row, sy, t, pressed == t);
        }
    }

    pv_disp_page_end();
}

static int tile_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int t = 0; t < 4; t++) {
        const int tx = tile_x(t), ty = tile_y(t);
        if (x >= tx && x < tx + TILE_W && y >= ty && y < ty + TILE_H) {
            return t + 1;
        }
    }
    return 0;
}

nui_action_t nui_menu_run(void)
{
    int pressed = -1;
    menu_draw(pressed);
    ESP_LOGI(TAG, "主菜单就绪（四格热区 %dx%d，间距 %d）",
             TILE_W, TILE_H, GRID_GAP);

    for (;;) {

        if (pv_touch_is_down()) {
            const int t = pv_touch_hit(tile_hit, NULL);
            if (t - 1 != pressed) {
                pressed = t - 1;
                menu_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ev.down) continue;

        const int t = pv_touch_hit(tile_hit, NULL);
        const int sel = (t > 0) ? (t - 1) : -1;
        pressed = -1;
        if (sel >= 0) {
            ESP_LOGI(TAG, "主菜单点了「%s」",
                     nui_action_name((nui_action_t)(sel + 1)));
            return (nui_action_t)(sel + 1);
        }
        menu_draw(pressed);
    }
}

const char *nui_action_name(nui_action_t a)
{
    switch (a) {
    case NUI_ACT_READ:    return "阅读";
    case NUI_ACT_IMAGE:   return "图片";
    case NUI_ACT_NET:     return "网络";
    case NUI_ACT_SETTING: return "设置";
    default:              return "无";
    }
}
