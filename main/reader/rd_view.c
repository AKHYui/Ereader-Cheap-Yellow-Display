#include "rd_view.h"

#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"
#include "lcd_st7789.h"
#include "net_remote.h"
#include "rd_bar.h"
#include "rd_book.h"
#include "rd_font.h"
#include "rd_list.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "rd_view";

#define IND_Y      254
#define IND_H      (RD_BAR_Y - IND_Y)

enum { B_UP = 1, B_BACK = 2, B_MARK = 3, B_DOWN = 4 };
static const char *const BAR4[4] = { "上页", "返回", "书签", "下页" };
static const char *const BAR3[3] = { "上页", "返回", "下页" };

#define POP_W       176
#define POP_H       132
#define POP_X       ((PV_SCR_W - POP_W) / 2)
#define POP_Y       94
#define POP_IX      (POP_X + 10)
#define POP_IW      (POP_W - 20)
#define POP_IH      40
#define POP_IY0     (POP_Y + 14)
#define POP_IY1     (POP_IY0 + POP_IH + 10)
#define POP_HINT_Y  (POP_IY1 + POP_IH + 6)

#define POP_SAVE    11
#define POP_LOAD    12

#define TOAST_W     168
#define TOAST_H       28
#define TOAST_X     ((PV_SCR_W - TOAST_W) / 2)
#define TOAST_Y      222
#define TOAST_MS    1200

static rd_page_t s_pg;

static bool    s_pop;
static int     s_toast;
static int64_t s_toast_t0;

static char s_jump[256];
static int  s_jump_page;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#define BM_NS      "reader"
#define BM_K_FILE  "bkname"
#define BM_K_PAGE  "bkpage"

static void bm_save(const char *file, int page)
{
    if (!file || !file[0]) return;

    nvs_handle_t h;
    if (nvs_open(BM_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "打不开 NVS，书签没存上");
        return;
    }
    esp_err_t r = nvs_set_str(h, BM_K_FILE, file);
    if (r == ESP_OK) r = nvs_set_u32(h, BM_K_PAGE, (uint32_t)page);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);

    if (r == ESP_OK) ESP_LOGI(TAG, "书签已保存：%s 第 %d 页", file, page + 1);
    else             ESP_LOGW(TAG, "书签写入失败: %s", esp_err_to_name(r));
}

static bool bm_load(char *file, size_t n, int *page)
{
    if (!file || !page) return false;

    nvs_handle_t h;
    if (nvs_open(BM_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t   len = n;
    uint32_t pg  = 0;
    const esp_err_t r1 = nvs_get_str(h, BM_K_FILE, file, &len);
    const esp_err_t r2 = nvs_get_u32(h, BM_K_PAGE, &pg);
    nvs_close(h);

    if (r1 != ESP_OK || r2 != ESP_OK || !file[0]) return false;
    *page = (int)pg;
    return true;
}

static void toast_row(uint16_t *row, int sy)
{
    if (!s_toast) return;
    if (sy < TOAST_Y || sy >= TOAST_Y + TOAST_H) return;

    const char    *s  = (s_toast == 1) ? "书签已保存" : "没有书签";
    const uint16_t fg = (s_toast == 1) ? NUI_OK : NUI_WARN;

    nui_frame_row(row, sy, TOAST_X, TOAST_Y, TOAST_W, TOAST_H, NUI_PANEL, NUI_EDGE, 2);

    uint32_t  cps[8];
    const int m = rd_cp_from_utf8(s, cps, 8);
    const int w = rd_font_width(cps, m);
    rd_font_row(row, sy, (PV_SCR_W - w) / 2, TOAST_Y + (TOAST_H - RD_CELL_H) / 2,
                cps, m, fg, NUI_PANEL);
}

static void popup_row(uint16_t *row, int sy, int sel)
{
    static const char *const label[2] = { "保存书签", "读取书签" };
    static const int         id[2]    = { POP_SAVE, POP_LOAD };

    if (sy >= POP_Y && sy < POP_Y + POP_H) {
        nui_frame_row(row, sy, POP_X, POP_Y, POP_W, POP_H, NUI_PANEL, NUI_EDGE, 2);
    }

    for (int i = 0; i < 2; i++) {
        const int      y  = i ? POP_IY1 : POP_IY0;
        const uint16_t bg = (sel == id[i]) ? NUI_PRESS : NUI_BG;

        nui_frame_row(row, sy, POP_IX, y, POP_IW, POP_IH, bg, NUI_EDGE, 2);
        nui_text_mid_center(row, sy, POP_IX + POP_IW / 2, y, POP_IH,
                            label[i], NUI_FG, bg);
    }

    uint32_t  cps[8];
    const int m = rd_cp_from_utf8("点空白处关闭", cps, 8);
    const int w = rd_font_width(cps, m);
    rd_font_row(row, sy, (PV_SCR_W - w) / 2, POP_HINT_Y, cps, m, NUI_DIM, NUI_PANEL);
}

#define PROG_TXT_Y   124
#define PROG_BAR_X    20
#define PROG_BAR_W   (PV_SCR_W - PROG_BAR_X * 2)
#define PROG_BAR_Y   164
#define PROG_BAR_H    18
#define PROG_NUM_Y   196

static void prog_draw(int done, int total)
{
    char ind[24];
    snprintf(ind, sizeof(ind), "%d/%d", done, total);

    pv_disp_page_begin(NUI_BG);
    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        uint32_t  cps[16];
        const int n = rd_cp_from_utf8("正在解析 EPUB", cps, 16);
        rd_font_row(row, sy, (PV_SCR_W - rd_font_width(cps, n)) / 2, PROG_TXT_Y,
                    cps, n, NUI_FG, NUI_BG);

        nui_frame_row(row, sy, PROG_BAR_X, PROG_BAR_Y, PROG_BAR_W, PROG_BAR_H,
                      NUI_PANEL, NUI_EDGE, 2);
        if (total > 0) {
            const int in_w = PROG_BAR_W - 4;
            int fill = in_w * done / total;
            if (fill < 0) fill = 0;
            if (fill > in_w) fill = in_w;
            if (fill > 0 && sy >= PROG_BAR_Y + 2 && sy < PROG_BAR_Y + PROG_BAR_H - 2) {
                nui_hspan(row, PROG_BAR_X + 2, PROG_BAR_X + 2 + fill, NUI_OK);
            }
        }

        rd_font_ascii_mid(row, sy, PV_SCR_W / 2, PROG_NUM_Y, ind, NUI_DIM, NUI_BG);
    }
    pv_disp_page_end();
}

static void view_draw(int pressed, int sel)
{
    char ind[24];
    snprintf(ind, sizeof(ind), "%d/%d", rd_book_page() + 1, rd_book_pages());

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        for (int L = 0; L < s_pg.lines; L++) {
            const int y_top = RD_TEXT_TOP + L * RD_LINE_H;
            if (sy < y_top || sy >= y_top + RD_CELL_H) continue;
            rd_font_row(row, sy, 0, y_top, s_pg.cp[L], s_pg.n[L], NUI_FG, NUI_BG);
        }

        rd_font_ascii_mid(row, sy, PV_SCR_W / 2, IND_Y + (IND_H - RD_CELL_H) / 2,
                          ind, NUI_DIM, NUI_BG);

        toast_row(row, sy);
        if (s_pop) popup_row(row, sy, sel);

        rd_bar_row(row, sy, 4, BAR4, pressed);
    }

    pv_disp_page_end();
}

static int popup_hit(int x, int y)
{
    if (x < POP_IX || x >= POP_IX + POP_IW) return 0;
    if (y >= POP_IY0 && y < POP_IY0 + POP_IH) return POP_SAVE;
    if (y >= POP_IY1 && y < POP_IY1 + POP_IH) return POP_LOAD;
    return 0;
}

static int view_hit(int x, int y, void *ctx)
{
    (void)ctx;
    if (s_pop) return popup_hit(x, y);
    return rd_bar_hit(x, y, 4);
}

static int err_hit(int x, int y, void *ctx)
{
    (void)ctx;
    return rd_bar_hit(x, y, 3);
}

static void move_page(int dir)
{
    const bool ok = (dir < 0) ? rd_book_prev() : rd_book_next();
    if (!ok) {

        ESP_LOGI(TAG, "%s", dir < 0 ? "已经是第 1 页" : "已经是最后一页");
        return;
    }
    if (!rd_book_load_page(&s_pg)) {
        ESP_LOGE(TAG, "取第 %d 页失败，退回上一页", rd_book_page() + 1);

        if (dir < 0) rd_book_next(); else rd_book_prev();
        rd_book_load_page(&s_pg);
    }
}

enum { LOOP_BACK = 0, LOOP_JUMP = 1 };

static int view_loop(void)
{
    int pressed = 0;
    int sel     = 0;

    s_pop    = false;
    s_toast  = 0;

    view_draw(pressed, sel);

    for (;;) {

        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(view_hit, NULL);
            if (s_pop) {
                if (h != sel) { sel = h; view_draw(pressed, sel); }
            } else if (h != pressed) {
                pressed = h;
                view_draw(pressed, sel);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {

            rc_cmd_t rc;
            int      rc_arg = 0;
            if (net_remote_take(&rc, &rc_arg)) {

                bool redraw = false;
                if (rc == RC_NEXT) {
                    move_page(+1);
                    redraw = true;
                } else if (rc == RC_PREV) {
                    move_page(-1);
                    redraw = true;
                } else if (rc == RC_BACK) {
                    return LOOP_BACK;
                } else if (rc == RC_BRIGHT) {
                    lcd_set_brightness(lcd_get_brightness() + rc_arg);
                } else if (rc == RC_GOTO &&
                           rd_book_goto(rc_arg) && rd_book_load_page(&s_pg)) {
                    redraw = true;
                }
                if (redraw) view_draw(pressed, sel);

                net_remote_sync(rd_book_file(), rd_book_page(), rd_book_pages());
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (!ev.down) {

            const int h = pv_touch_hit(view_hit, NULL);

            if (s_pop) {
                sel   = 0;
                s_pop = false;

                if (h == POP_SAVE) {
                    bm_save(rd_book_file(), rd_book_page());
                    s_toast = 1;
                    s_toast_t0 = now_ms();
                } else if (h == POP_LOAD) {
                    if (bm_load(s_jump, sizeof(s_jump), &s_jump_page)) {
                        return LOOP_JUMP;
                    }
                    s_toast = 2;
                    s_toast_t0 = now_ms();
                }

                view_draw(0, 0);
            } else {
                pressed = 0;

                if (h == B_UP) {
                    move_page(-1);
                } else if (h == B_BACK) {
                    return LOOP_BACK;
                } else if (h == B_MARK) {
                    s_pop = true;
                    view_draw(0, 0);
                } else if (h == B_DOWN) {
                    move_page(+1);
                }

                view_draw(pressed, sel);
            }
        }

        if (s_toast && now_ms() - s_toast_t0 >= TOAST_MS) {
            s_toast = 0;
            view_draw(pressed, sel);
        }

        net_remote_sync(rd_book_file(), rd_book_page(), rd_book_pages());
    }
}

static void show_error(void)
{

    int shown = -1;

    for (;;) {

        const int cur = pv_touch_is_down() ? pv_touch_hit(err_hit, NULL) : 0;
        if (cur != shown) {
            shown = cur;
            pv_disp_page_begin(NUI_BG);
            for (int sy = 0; sy < PV_SCR_H; sy++) {
                uint16_t *row = pv_disp_page_row(sy);
                if (!row) continue;
                nui_text_mid_center(row, sy, PV_SCR_W / 2, 116, 40,
                                    "打开失败", NUI_ERR, NUI_BG);
                rd_bar_row(row, sy, 3, BAR3, shown);
            }
            pv_disp_page_end();
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        if (pv_touch_hit(err_hit, NULL) == 2) return;

    }
}

void rd_view_run(const char *path)
{
    rd_view_run_at(path, 0);
}

void rd_view_run_at(const char *path, int page)
{
    if (!path) return;

    static char cur[RD_LIST_DIR_LEN + 1 + 256];
    snprintf(cur, sizeof(cur), "%s", path);
    int start = page;

    for (;;) {
        if (!rd_book_open(cur, prog_draw)) {
            rd_book_close();
            show_error();
            return;
        }

        if (start > 0) {
            int p = start;
            if (p >= rd_book_pages()) p = rd_book_pages() - 1;
            if (p < 0) p = 0;
            rd_book_goto(p);
            ESP_LOGI(TAG, "跳到第 %d 页（共 %d 页）", p + 1, rd_book_pages());
        }

        if (!rd_book_load_page(&s_pg)) {
            ESP_LOGE(TAG, "取第 %d 页失败", rd_book_page() + 1);
            rd_book_close();
            show_error();
            return;
        }

        if (view_loop() == LOOP_BACK) {
            rd_book_close();
            net_remote_sync(NULL, 0, 0);
            return;
        }

        rd_book_close();
        snprintf(cur, sizeof(cur), RD_LIST_DIR "/%s", s_jump);
        start = s_jump_page;
        ESP_LOGI(TAG, "按书签打开 %s（第 %d 页）", cur, start + 1);
    }
}
