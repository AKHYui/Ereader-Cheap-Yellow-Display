#include "nui_sec.h"

#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "nui_sec";

#define SEC_NS     "security"
#define SEC_K_EN   "en"
#define SEC_K_PIN  "pin"

#define PIN_LEN    6

static bool sec_load(uint32_t *pin)
{
    nvs_handle_t h;
    if (nvs_open(SEC_NS, NVS_READONLY, &h) != ESP_OK) return false;

    uint8_t  en = 0;
    uint32_t v  = 0;
    const esp_err_t r1 = nvs_get_u8(h, SEC_K_EN, &en);
    const esp_err_t r2 = nvs_get_u32(h, SEC_K_PIN, &v);
    nvs_close(h);

    if (r1 != ESP_OK || r2 != ESP_OK || en == 0) return false;
    if (pin) *pin = v;
    return true;
}

static void sec_store(uint32_t pin)
{
    nvs_handle_t h;
    if (nvs_open(SEC_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "打不开 NVS，密码没存上");
        return;
    }
    esp_err_t r = nvs_set_u32(h, SEC_K_PIN, pin);
    if (r == ESP_OK) r = nvs_set_u8(h, SEC_K_EN, 1);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);

    if (r == ESP_OK) ESP_LOGI(TAG, "开机密码已保存（%d 位，已启用）", PIN_LEN);
    else             ESP_LOGE(TAG, "开机密码写入失败: %s", esp_err_to_name(r));
}

static void sec_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(SEC_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, SEC_K_PIN);
    nvs_set_u8(h, SEC_K_EN, 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "开机密码已关闭并清除");
}

bool nui_sec_enabled(void)
{
    return sec_load(NULL);
}

#define PK_BACK_X   6
#define PK_BACK_Y   6
#define PK_BACK_W   80
#define PK_BACK_H   44

#define PK_HINT_X   90
#define PK_HINT_W   (PV_SCR_W - PK_HINT_X - 4)

#define PK_CELL_W   32
#define PK_CELL_H   36
#define PK_CELL_GAP  4
#define PK_CELL_Y   68
#define PK_CELL_X   ((PV_SCR_W - (PIN_LEN * PK_CELL_W + (PIN_LEN - 1) * PK_CELL_GAP)) / 2)

#define PK_KEY_W    72
#define PK_KEY_H    44
#define PK_KEY_GAP   6
#define PK_COLS      3
#define PK_ROWS      4
#define PK_KB_X     6
#define PK_KB_Y     112

enum { K_DEL = 9, K_ZERO = 10, K_OK = 11, K_N = 12 };

static const char *const PK_LABEL[K_N] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "删除", "0", "确定",
};

#define PK_BACK  (K_N + 1)

#define KEY_ID(k)  ((k) + 1)

static uint8_t s_buf[PIN_LEN];
static int     s_len;

static bool s_allow_cancel;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int pk_key_x(int c) { return PK_KB_X + c * (PK_KEY_W + PK_KEY_GAP); }
static int pk_key_y(int r) { return PK_KB_Y + r * (PK_KEY_H + PK_KEY_GAP); }

static void dot_row(uint16_t *row, int sy, int cx, int cy, int r, uint16_t c)
{
    const int dy = sy - cy;
    if (dy < -r || dy > r) return;

    int hw = 0;
    while ((hw + 1) * (hw + 1) <= r * r - dy * dy) hw++;
    nui_hspan(row, cx - hw, cx + hw + 1, c);
}

static void pin_draw(const char *title, const char *err, bool allow_cancel, int pressed)
{
    const bool     show_err = (err && s_len == 0);
    const char    *hint     = show_err ? err : title;
    const uint16_t hfg      = show_err ? NUI_ERR : NUI_FG;

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        if (allow_cancel) {
            nui_button_row(row, sy, PK_BACK_X, PK_BACK_Y, PK_BACK_W, PK_BACK_H,
                           "返回", pressed == PK_BACK);
        }

        {
            const int cx = allow_cancel ? (PK_HINT_X + PK_HINT_W / 2) : (PV_SCR_W / 2);
            nui_text_mid_center(row, sy, cx, PK_BACK_Y, PK_BACK_H, hint, hfg, NUI_BG);
        }

        for (int i = 0; i < PIN_LEN; i++) {
            const int x = PK_CELL_X + i * (PK_CELL_W + PK_CELL_GAP);
            nui_frame_row(row, sy, x, PK_CELL_Y, PK_CELL_W, PK_CELL_H,
                          NUI_PANEL, NUI_EDGE, 2);
            if (i < s_len) {
                dot_row(row, sy, x + PK_CELL_W / 2, PK_CELL_Y + PK_CELL_H / 2, 5, NUI_FG);
            }
        }

        for (int k = 0; k < K_N; k++) {
            const int x = pk_key_x(k % PK_COLS);
            const int y = pk_key_y(k / PK_COLS);
            if (sy < y || sy >= y + PK_KEY_H) continue;

            const uint16_t bg = (pressed == KEY_ID(k)) ? NUI_PRESS : NUI_PANEL;
            nui_frame_row(row, sy, x, y, PK_KEY_W, PK_KEY_H, bg, NUI_EDGE, 2);

            const char *lbl = PK_LABEL[k];
            if (k == K_DEL || k == K_OK) {

                nui_text_mid_center(row, sy, x + PK_KEY_W / 2, y, PK_KEY_H,
                                    lbl, NUI_FG, bg);
            } else {

                const int w = nui_ascii2x_w(lbl);
                nui_ascii2x_row(row, sy, x + (PK_KEY_W - w) / 2,
                                y + (PK_KEY_H - 32) / 2, lbl, NUI_FG);
            }
        }
    }

    pv_disp_page_end();
}

static int pin_hit(int x, int y, void *ctx)
{
    (void)ctx;

    if (s_allow_cancel &&
        x >= PK_BACK_X && x < PK_BACK_X + PK_BACK_W &&
        y >= PK_BACK_Y && y < PK_BACK_Y + PK_BACK_H) {
        return PK_BACK;
    }

    for (int k = 0; k < K_N; k++) {
        const int kx = pk_key_x(k % PK_COLS);
        const int ky = pk_key_y(k / PK_COLS);
        if (x >= kx && x < kx + PK_KEY_W && y >= ky && y < ky + PK_KEY_H) {
            return KEY_ID(k);
        }
    }
    return 0;
}

static int pin_prompt(const char *title, const char *err, bool allow_cancel,
                      uint32_t *out)
{
    s_len = 0;
    s_allow_cancel = allow_cancel;

    int pressed = 0;
    pin_draw(title, err, allow_cancel, pressed);

    for (;;) {

        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(pin_hit, NULL);
            if (h != pressed) {
                pressed = h;
                pin_draw(title, err, allow_cancel, pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(pin_hit, NULL);
        pressed = 0;

        if (h == PK_BACK) {
            return 0;
        }
        if (h < 1 || h > K_N) {
            pin_draw(title, err, allow_cancel, pressed);
            continue;
        }

        const int k = h - 1;
        if (k == K_DEL) {
            if (s_len > 0) s_len--;
        } else if (k == K_OK) {
            if (s_len == PIN_LEN) {
                uint32_t v = 0;
                for (int i = 0; i < PIN_LEN; i++) v = v * 10u + s_buf[i];
                if (out) *out = v;
                return 1;
            }

            ESP_LOGI(TAG, "确定键：还差 %d 位", PIN_LEN - s_len);
        } else {
            const int d = (k == K_ZERO) ? 0 : (k + 1);
            if (s_len < PIN_LEN) s_buf[s_len++] = (uint8_t)d;
        }

        pin_draw(title, err, allow_cancel, pressed);
    }
}

bool nui_sec_unlock(void)
{
    uint32_t pin = 0;
    if (!sec_load(&pin)) return true;

    ESP_LOGW(TAG, "开机密码已启用，等待解锁");

    const char *err = NULL;
    for (int tries = 0; ; tries++) {
        uint32_t got = 0;
        if (!pin_prompt("输入密码", err, false, &got)) continue;

        if (got == pin) {
            ESP_LOGI(TAG, "开机密码正确，进入主菜单");
            return true;
        }

        ESP_LOGW(TAG, "开机密码错误（第 %d 次）", tries + 1);
        err = "密码错误";
    }
}

enum { SS_NONE = 0, SS_TOGGLE = 1, SS_CHANGE = 2, SS_BACK = 3 };

_Static_assert(SS_BACK - SS_TOGGLE == 2, "SS_* 必须与条目顺序一一对应且连续");

#define ITEMS       3

#define ST_Y        262
#define ST_H        48

#define TOAST_MS    1400

static const char *s_toast;
static int64_t     s_toast_t0;

static void toast(const char *s)
{
    s_toast    = s;
    s_toast_t0 = now_ms();
}

static void ss_draw(int pressed)
{
    const bool on = nui_sec_enabled();

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "设备安全");

        {
            const uint16_t bg = nui_item_row(row, sy, 0, pressed == 0);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(0), NUI_ITEM_H,
                         on ? "关闭密码" : "开启密码",
                         on ? NUI_WARN : NUI_OK, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 1, pressed == 1);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(1), NUI_ITEM_H,
                         "修改密码", NUI_FG, bg);
        }

        {
            const uint16_t bg = nui_item_row(row, sy, 2, pressed == 2);
            nui_text_mid(row, sy, NUI_ITEM_X + 18, nui_item_y(2), NUI_ITEM_H,
                         "返回", NUI_FG, bg);
        }

        if (s_toast) {
            nui_text_mid_center(row, sy, PV_SCR_W / 2, ST_Y, ST_H,
                                s_toast, NUI_WARN, NUI_BG);
        } else {
            nui_text_mid_center(row, sy, PV_SCR_W / 2, ST_Y, ST_H,
                                on ? "开机密码已开启" : "开机密码未开启",
                                on ? NUI_OK : NUI_DIM, NUI_BG);
        }
    }

    pv_disp_page_end();
}

static int ss_hit(int x, int y, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < ITEMS; i++) {
        const int iy = nui_item_y(i);
        if (x >= NUI_ITEM_X && x < NUI_ITEM_X + NUI_ITEM_W &&
            y >= iy && y < iy + NUI_ITEM_H) {
            return SS_TOGGLE + i;
        }
    }
    return SS_NONE;
}

static bool set_new_pin(void)
{
    uint32_t a = 0, b = 0;

    if (!pin_prompt("设置密码", NULL, true, &a)) return false;
    if (!pin_prompt("再输一次", NULL, true, &b)) return false;

    if (a != b) {
        ESP_LOGW(TAG, "两次输入不一致，未保存");
        toast("不一致");
        return false;
    }

    sec_store(a);
    return true;
}

void nui_sec_run(void)
{
    int pressed = -1;
    s_toast = NULL;
    ss_draw(pressed);
    ESP_LOGI(TAG, "设备安全页就绪（当前%s）",
             nui_sec_enabled() ? "已开启" : "未开启");

    for (;;) {

        if (s_toast && now_ms() - s_toast_t0 >= TOAST_MS) {
            s_toast = NULL;
            ss_draw(pressed);
        }

        if (pv_touch_is_down()) {
            const int h   = pv_touch_hit(ss_hit, NULL);
            const int idx = (h == SS_NONE) ? -1 : (h - SS_TOGGLE);
            if (idx != pressed) {
                pressed = idx;
                ss_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h   = pv_touch_hit(ss_hit, NULL);
        const int idx = h - SS_TOGGLE;
        pressed = -1;

        if (idx == 0) {
            if (nui_sec_enabled()) {

                sec_clear();
            } else {
                set_new_pin();
            }
            ss_draw(pressed);
        } else if (idx == 1) {

            if (set_new_pin()) ESP_LOGI(TAG, "密码已更新");
            ss_draw(pressed);
        } else if (idx == 2) {
            return;
        } else {
            ss_draw(pressed);
        }
    }
}
