#include "nui_setting.h"

#include "lcd_st7789.h"
#include "net_wifi.h"
#include "nui_sec.h"
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

static const char *TAG = PV_TAG;

enum { SH_NONE = 0, SH_SEC = 1, SH_BRI = 2, SH_FORGET = 3, SH_BACK = 4 };

_Static_assert(SH_BACK - SH_SEC == 3, "SH_* 必须与条目顺序一一对应且连续");

#define SET_ITEMS   4
#define FLASH_MS 1200

static bool    s_flash;
static int64_t s_flash_t0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#define BRI_STEP     5
#define BRI_NUM_Y   76
#define BRI_BAR_X   20
#define BRI_BAR_W   (PV_SCR_W - BRI_BAR_X * 2)
#define BRI_BAR_Y   148
#define BRI_BAR_H   26
#define BRI_HINT_Y  186
#define BRI_BTN_W   56
#define BRI_BTN_H   44
#define BRI_BTN_Y   250

enum { BR_NONE = 0, BR_DOWN = 1, BR_BACK = 2, BR_UP = 3 };

#define BRI_BTN_X(i)  (BRI_BAR_X + (i) * (BRI_BTN_W + 16))

static void bri_draw(int pressed)
{
    char num[8];
    snprintf(num, sizeof(num), "%d%%", lcd_get_brightness());

    const int pct = lcd_get_brightness();

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "亮度");

        {
            const int w = nui_ascii2x_w(num);
            nui_ascii2x_row(row, sy, (PV_SCR_W - w) / 2, BRI_NUM_Y, num, NUI_FG);
        }

        nui_frame_row(row, sy, BRI_BAR_X, BRI_BAR_Y, BRI_BAR_W, BRI_BAR_H,
                      NUI_PANEL, NUI_EDGE, 2);
        {
            const int in_x = BRI_BAR_X + 2;
            const int in_w = BRI_BAR_W - 4;
            const int fill = in_w * pct / 100;
            if (fill > 0 && sy >= BRI_BAR_Y + 2 && sy < BRI_BAR_Y + BRI_BAR_H - 2) {
                nui_hspan(row, in_x, in_x + fill, NUI_OK);
            }
        }

        nui_text_mid_center(row, sy, PV_SCR_W / 2, BRI_HINT_Y, 32,
                            "1% - 100%", NUI_DIM, NUI_BG);

        static const char *const LBL[3] = { "-", "返回", "+" };
        for (int i = 0; i < 3; i++) {
            nui_button_row(row, sy, BRI_BTN_X(i), BRI_BTN_Y, BRI_BTN_W, BRI_BTN_H,
                           LBL[i], pressed == (i + 1));
        }
    }

    pv_disp_page_end();
}

static int bri_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < 3; i++) {
        const int bx = BRI_BTN_X(i);
        if (x >= bx && x < bx + BRI_BTN_W &&
            y >= BRI_BTN_Y && y < BRI_BTN_Y + BRI_BTN_H) {
            return i + 1;
        }
    }
    return BR_NONE;
}

static void bri_run(void)
{
    int pressed = 0;
    bri_draw(pressed);
    ESP_LOGI(TAG, "亮度页就绪（当前 %d%%，步进 %d%%）",
             lcd_get_brightness(), BRI_STEP);

    for (;;) {
        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(bri_hit, NULL);
            if (h != pressed) {
                pressed = h;
                bri_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(bri_hit, NULL);
        pressed = 0;

        if (h == BR_DOWN) {
            lcd_set_brightness(lcd_get_brightness() - BRI_STEP);
        } else if (h == BR_UP) {
            lcd_set_brightness(lcd_get_brightness() + BRI_STEP);
        } else if (h == BR_BACK) {
            lcd_bl_save();
            return;
        }

        bri_draw(pressed);
    }
}

static void setting_draw(int pressed)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "设置");

        {
            const uint16_t bg = nui_item_row(row, sy, 0, pressed == 0);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(0), NUI_ITEM_H,
                         "设备安全", NUI_FG, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 1, pressed == 1);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(1), NUI_ITEM_H,
                         "亮度调节", NUI_FG, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 2, pressed == 2);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(2), NUI_ITEM_H,
                         s_flash ? "已删除" : "忘记网络",
                         s_flash ? NUI_OK : NUI_WARN, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 3, pressed == 3);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(3), NUI_ITEM_H,
                         "返回", NUI_FG, bg);
        }
    }

    pv_disp_page_end();
}

static int setting_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < SET_ITEMS; i++) {
        const int iy = nui_item_y(i);
        if (x >= NUI_ITEM_X && x < NUI_ITEM_X + NUI_ITEM_W &&
            y >= iy && y < iy + NUI_ITEM_H) {
            return SH_SEC + i;
        }
    }
    return SH_NONE;
}

void nui_setting_run(void)
{
    int pressed = -1;
    s_flash = false;
    setting_draw(pressed);
    ESP_LOGI(TAG, "设置页就绪（设备安全 / 亮度调节 / 忘记网络 / 返回）");

    for (;;) {

        if (s_flash && now_ms() - s_flash_t0 > FLASH_MS) {
            s_flash = false;
            setting_draw(pressed);
        }

        if (pv_touch_is_down()) {
            const int h   = pv_touch_hit(setting_hit, NULL);
            const int idx = (h == SH_NONE) ? -1 : (h - SH_SEC);
            if (idx != pressed) {
                pressed = idx;
                setting_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ev.down) continue;

        const int h   = pv_touch_hit(setting_hit, NULL);
        const int idx = h - SH_SEC;
        pressed = -1;

        if (idx == 0) {
            nui_sec_run();
            setting_draw(pressed);
        } else if (idx == 1) {
            bri_run();
            setting_draw(pressed);
        } else if (idx == 2) {

            net_wifi_disconnect();
            net_wifi_forget();
            s_flash    = true;
            s_flash_t0 = now_ms();
            ESP_LOGI(TAG, "用户点了「忘记网络」");
            setting_draw(pressed);
        } else if (idx == 3) {
            return;
        } else {
            setting_draw(pressed);
        }
    }
}
