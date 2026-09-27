#include "nui_setting.h"

#include "lcd_st7789.h"
#include "net_wifi.h"
#include "nui_diag.h"
#include "nui_led.h"
#include "nui_sec.h"
#include "nui_sleep.h"
#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"
#include "rd_font.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char *TAG = PV_TAG;

enum { SH_NONE = 0, SH_SEC = 1, SH_BRI = 2, SH_DIAG = 3, SH_FORGET = 4, SH_BACK = 5 };

_Static_assert(SH_BACK - SH_SEC == 4, "SH_* 必须与条目顺序一一对应且连续");

#define SET_ITEMS   5
#define FLASH_MS 1200

static bool    s_flash;
static int64_t s_flash_t0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#define BRI_STEP     5
#define BRI_NUM_Y   52
#define BRI_BAR_X   20
#define BRI_BAR_W   (PV_SCR_W - BRI_BAR_X * 2)
#define BRI_BAR_Y   96
#define BRI_BAR_H   24
#define BRI_HINT_Y  124
#define BRI_BTN_W   56
#define BRI_BTN_H   48
#define BRI_BTN_Y   164

#define LED_ROW_Y   220
#define LED_ROW_H    44
#define SLP_ROW_Y   272
#define SLP_ROW_H    44

enum { BR_NONE = 0, BR_DOWN = 1, BR_BACK = 2, BR_UP = 3, BR_LED = 4, BR_SLP = 5 };

static void sleep_run(void);

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

        nui_title_row(row, sy, "显示");

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

        {
            static char lb[32];

            snprintf(lb, sizeof(lb), "指示灯 %s", nui_led_enabled() ? "开" : "关");
            nui_button_row(row, sy, BRI_BAR_X, LED_ROW_Y, BRI_BAR_W, LED_ROW_H,
                           lb, pressed == BR_LED);
        }

        nui_button_row(row, sy, BRI_BAR_X, SLP_ROW_Y, BRI_BAR_W, SLP_ROW_H,
                       "屏保管理", pressed == BR_SLP);
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
    if (x >= BRI_BAR_X && x < BRI_BAR_X + BRI_BAR_W) {
        if (y >= LED_ROW_Y && y < LED_ROW_Y + LED_ROW_H) return BR_LED;
        if (y >= SLP_ROW_Y && y < SLP_ROW_Y + SLP_ROW_H) return BR_SLP;
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

        if (nui_sleep_poll()) {
            pressed = 0;
            bri_draw(pressed);
            continue;
        }

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

        if (h == BR_DOWN || h == BR_UP) {
            lcd_set_brightness(lcd_get_brightness()
                               + (h == BR_UP ? BRI_STEP : -BRI_STEP));
        } else if (h == BR_LED) {

            nui_led_set_enabled(!nui_led_enabled());
        } else if (h == BR_SLP) {
            sleep_run();
        } else if (h == BR_BACK) {
            lcd_bl_save();
            return;
        }

        bri_draw(pressed);
    }
}

enum { SL_NONE = 0, SL_TO = 1, SL_STYLE = 2, SL_BACK = 3 };
#define SL_ROWS 3

#define SLP_HINT_Y  216

static void sleep_row_text(uint16_t *row, int sy, int i, const char *label,
                           const char *val, uint16_t vcol, bool pressed)
{
    const uint16_t bg = nui_crow(row, sy, i, pressed);
    nui_ctext(row, sy, i, label, NUI_FG, bg);

    nui_text_mid(row, sy, NUI_CX + NUI_CW - 18 - nui_text_w(val),
                 nui_cy(i), NUI_CH, val, vcol, bg);
}

static void sleep_draw(int pressed)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "屏保管理");

        sleep_row_text(row, sy, 0, "屏保", nui_sleep_timeout_text(),
                       nui_sleep_timeout() > 0 ? NUI_OK : NUI_DIM, pressed == 0);
        sleep_row_text(row, sy, 1, "样式", nui_sleep_style_text(),
                       NUI_OK, pressed == 1);
        nui_ctext(row, sy, 2, "返回", NUI_FG, nui_crow(row, sy, 2, pressed == 2));

        nui_hint_row(row, sy, SLP_HINT_Y,      "屏保1 需要联网取时间", NUI_DIM, NUI_BG);
        nui_hint_row(row, sy, SLP_HINT_Y + 24, "屏保2 像素鲸鱼娘动画", NUI_DIM, NUI_BG);
        nui_hint_row(row, sy, SLP_HINT_Y + 48, "无网络时自动用屏保2", NUI_WARN, NUI_BG);
    }

    pv_disp_page_end();
}

static int sleep_hit(int x, int y, void *ctx)
{
    (void)ctx;
    if (x < NUI_CX || x >= NUI_CX + NUI_CW) return SL_NONE;
    for (int i = 0; i < SL_ROWS; i++) {
        const int iy = nui_cy(i);
        if (y >= iy && y < iy + NUI_CH) return SL_TO + i;
    }
    return SL_NONE;
}

static void sleep_run(void)
{
    int pressed = -1;
    sleep_draw(pressed);
    ESP_LOGI(TAG, "屏保管理页就绪（%s / %s）",
             nui_sleep_timeout_text(), nui_sleep_style_text());

    for (;;) {

        if (nui_sleep_poll()) {
            pressed = -1;
            sleep_draw(pressed);
            continue;
        }

        if (pv_touch_is_down()) {
            const int h   = pv_touch_hit(sleep_hit, NULL);
            const int idx = (h == SL_NONE) ? -1 : (h - SL_TO);
            if (idx != pressed) {
                pressed = idx;
                sleep_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(sleep_hit, NULL);
        pressed = -1;

        if (h == SL_TO)         nui_sleep_next_timeout();
        else if (h == SL_STYLE) nui_sleep_next_style();
        else if (h == SL_BACK)  return;

        sleep_draw(pressed);
    }
}

static void setting_draw(int pressed)
{
    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "设置");

        nui_ctext(row, sy, 0, "设备安全", NUI_FG, nui_crow(row, sy, 0, pressed == 0));
        nui_ctext(row, sy, 1, "显示/屏保", NUI_FG, nui_crow(row, sy, 1, pressed == 1));
        nui_ctext(row, sy, 2, "芯片体检", NUI_FG, nui_crow(row, sy, 2, pressed == 2));

        nui_ctext(row, sy, 3, s_flash ? "已删除" : "忘记网络",
                  s_flash ? NUI_OK : NUI_WARN, nui_crow(row, sy, 3, pressed == 3));

        nui_ctext(row, sy, 4, "返回", NUI_FG, nui_crow(row, sy, 4, pressed == 4));
    }

    pv_disp_page_end();
}

static int setting_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < SET_ITEMS; i++) {
        const int iy = nui_cy(i);
        if (x >= NUI_CX && x < NUI_CX + NUI_CW &&
            y >= iy && y < iy + NUI_CH) {
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
    ESP_LOGI(TAG, "设置页就绪（设备安全 / 显示与屏保 / 芯片体检 / 忘记网络 / 返回）");

    for (;;) {

        if (nui_sleep_poll()) {
            pressed = -1;
            setting_draw(pressed);
            continue;
        }

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
            nui_diag_run();
            setting_draw(pressed);
        } else if (idx == 3) {

            net_wifi_disconnect();
            net_wifi_forget();
            s_flash    = true;
            s_flash_t0 = now_ms();
            ESP_LOGI(TAG, "用户点了「忘记网络」");
            setting_draw(pressed);
        } else if (idx == 4) {
            return;
        } else {
            setting_draw(pressed);
        }
    }
}
