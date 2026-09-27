#include "rd_list.h"

#include "nui_ui.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "pv_touch.h"
#include "rd_bar.h"
#include "rd_book.h"
#include "rd_font.h"
#include "rd_txt.h"
#include "rd_view.h"

#if RD_BOOT_SELFTEST
#include "esp_timer.h"
#include "net_wifi.h"
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "rd_list";

#define RD_LIST_MAX      192

#define RD_LIST_POOL   12288
#define RD_NAME_U8_MAX   384
#define RD_PATH_MAX      288

#define RD_LIST_PER_PAGE   6

#define ITEM_X      8
#define ITEM_W    224
#define ITEM_H     32
#define ITEM_PITCH 36
#define ITEM_Y0    52

#define HIT_ITEM_BASE 10

static const char *const BAR3[3] = { "上页", "返回", "下页" };

static char    *s_pool;
static size_t   s_pool_used;
static uint32_t s_off[RD_LIST_MAX];
static int      s_n;
static int      s_pg;

static const char *name_of(int i)
{
    return (i >= 0 && i < s_n) ? &s_pool[s_off[i]] : "";
}

static int cmp_off(const void *a, const void *b)
{
    return strcmp(&s_pool[*(const uint32_t *)a], &s_pool[*(const uint32_t *)b]);
}

static void scan_novels(void)
{
    s_n = 0;
    s_pool_used = 0;
    if (!s_pool) {
        s_pool = heap_caps_malloc(RD_LIST_POOL, MALLOC_CAP_8BIT);
        if (!s_pool) {
            ESP_LOGE(TAG, "名字池分配失败（%d 字节）", RD_LIST_POOL);
            return;
        }
    }

    if (mkdir(RD_LIST_DIR, 0777) != 0) {

    }

    DIR *d = opendir(RD_LIST_DIR);
    if (!d) {
        ESP_LOGW(TAG, "打不开 %s —— 卡没挂载或目录建不出来", RD_LIST_DIR);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !rd_book_path_is_book(e->d_name)) continue;
        if (s_n >= RD_LIST_MAX) {
            ESP_LOGW(TAG, "书超过 %d 本，只列前 %d 本", RD_LIST_MAX, RD_LIST_MAX);
            break;
        }

        const size_t nl = strlen(e->d_name);
        if (s_pool_used + nl + 1 > RD_LIST_POOL) {

            ESP_LOGW(TAG, "名字池满（%d 字节），从「%s」起不再列出（已列 %d 本）",
                     RD_LIST_POOL, e->d_name, s_n);
            break;
        }

        s_off[s_n] = (uint32_t)s_pool_used;
        memcpy(&s_pool[s_pool_used], e->d_name, nl + 1);
        s_pool_used += nl + 1;
        s_n++;
    }
    closedir(d);

    if (s_n > 1) qsort(s_off, (size_t)s_n, sizeof(uint32_t), cmp_off);
    ESP_LOGI(TAG, "书籍列表：%s 下 %d 本（TXT+EPUB，名字池用 %u/%d 字节）",
             RD_LIST_DIR, s_n, (unsigned)s_pool_used, RD_LIST_POOL);
}

static int page_count(void)
{
    const int p = (s_n + RD_LIST_PER_PAGE - 1) / RD_LIST_PER_PAGE;
    return p ? p : 1;
}

static void free_pool(void)
{
    if (s_pool) {
        heap_caps_free(s_pool);
        s_pool = NULL;
    }
    s_pool_used = 0;
    s_n = 0;
}

static void list_draw(int pressed)
{
    char ind[24];
    snprintf(ind, sizeof(ind), "%d/%d", s_pg + 1, page_count());

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_title_row(row, sy, "小说");

        rd_font_ascii_row(row, sy, PV_SCR_W - 8 - rd_font_ascii_w(ind),
                          NUI_TITLE_Y + (NUI_TITLE_H - RD_CELL_H) / 2,
                          ind, NUI_DIM, NUI_BG);

        if (s_n == 0) {
            nui_text_mid_center(row, sy, PV_SCR_W / 2, 116, 40,
                                "未找到文件", NUI_DIM, NUI_BG);

            rd_font_ascii_mid(row, sy, PV_SCR_W / 2, 166, "txt / epub", NUI_DIM, NUI_BG);
        } else {
            for (int i = 0; i < RD_LIST_PER_PAGE; i++) {
                const int idx = s_pg * RD_LIST_PER_PAGE + i;
                if (idx >= s_n) break;

                const int y = ITEM_Y0 + i * ITEM_PITCH;
                if (sy < y || sy >= y + ITEM_H) continue;

                const uint16_t bg = (pressed == HIT_ITEM_BASE + i) ? NUI_PRESS : NUI_PANEL;
                nui_frame_row(row, sy, ITEM_X, y, ITEM_W, ITEM_H, bg, NUI_EDGE, 2);

                static char u8[RD_NAME_U8_MAX];
                uint32_t cps[64];
                rd_name_utf8(name_of(idx), u8, (int)sizeof(u8));
                int n = rd_cp_from_utf8(u8, cps, 64);
                n = rd_cp_fit(cps, n, ITEM_W - 16);

                rd_font_row(row, sy, ITEM_X + 8, y + (ITEM_H - RD_CELL_H) / 2,
                            cps, n, NUI_FG, bg);
            }
        }

        rd_bar_row(row, sy, 3, BAR3, pressed);    }

    pv_disp_page_end();
}

static int list_hit(int x, int y, void *ctx)
{
    (void)ctx;

    const int bar = rd_bar_hit(x, y, 3);
    if (bar) return bar;

    if (s_n == 0) return 0;
    if (x < ITEM_X || x >= ITEM_X + ITEM_W) return 0;

    for (int i = 0; i < RD_LIST_PER_PAGE; i++) {
        if (s_pg * RD_LIST_PER_PAGE + i >= s_n) break;
        const int y0 = ITEM_Y0 + i * ITEM_PITCH;
        if (y >= y0 && y < y0 + ITEM_H) return HIT_ITEM_BASE + i;
    }
    return 0;
}

void rd_list_run(void)
{
    scan_novels();
    s_pg = 0;

    int  pressed = 0;
    bool back    = false;

    list_draw(pressed);

    while (!back) {

        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(list_hit, NULL);
            if (h != pressed) {
                pressed = h;
                list_draw(pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(list_hit, NULL);
        pressed = 0;

        if (h == 1) {
            if (s_pg > 0) s_pg--;
        } else if (h == 2) {
            back = true;
            continue;
        } else if (h == 3) {
            if (s_pg + 1 < page_count()) s_pg++;
        } else if (h >= HIT_ITEM_BASE) {
            const int idx = s_pg * RD_LIST_PER_PAGE + (h - HIT_ITEM_BASE);
            if (idx < s_n) {
                static char path[RD_PATH_MAX];
                snprintf(path, sizeof(path), RD_LIST_DIR "/%s", name_of(idx));

                free_pool();

                rd_view_run(path);

                scan_novels();

                if (s_n == 0) {
                    back = true;
                    continue;
                }
                const int last = page_count() - 1;
                if (s_pg > last) s_pg = last;
                if (s_pg < 0)    s_pg = 0;
            }
        }

        list_draw(pressed);
    }

    free_pool();
}

#if RD_BOOT_SELFTEST

static void log_cps(const uint32_t *cp, int n, int max)
{
    char buf[256];
    int  m = 0;
    for (int i = 0; i < n && i < max && m < (int)sizeof(buf) - 4; i++) {
        const uint32_t c = cp[i];
        if (c < 0x80) {
            buf[m++] = (char)c;
        } else if (c < 0x800) {
            buf[m++] = (char)(0xC0 | (c >> 6));
            buf[m++] = (char)(0x80 | (c & 0x3F));
        } else {
            buf[m++] = (char)(0xE0 | (c >> 12));
            buf[m++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[m++] = (char)(0x80 | (c & 0x3F));
        }
    }
    buf[m] = 0;

    ESP_LOGI(TAG, "      |%s|", buf);
}

void rd_selftest(void)
{
    ESP_LOGI(TAG, "=== 阅读功能自检 ===");

    if (!rd_font_ok()) {
        ESP_LOGE(TAG, "字库没装载成功 —— 后面的验证没有意义，先查 rd_font16.bin");
        return;
    }

    scan_novels();
    ESP_LOGI(TAG, "扫描 %s：%d 本", RD_LIST_DIR, s_n);
    for (int i = 0; i < s_n && i < 8; i++) {
        static char u8[RD_NAME_U8_MAX];
        rd_name_utf8(name_of(i), u8, (int)sizeof(u8));
        ESP_LOGI(TAG, "  [%d] %s", i, u8);
    }
    if (s_n == 0) {
        ESP_LOGW(TAG, "目录里没有书 —— 先用网页「文件接收」选 novels 上传一本再重跑");
        ESP_LOGI(TAG, "=== 自检结束 ===");
        return;
    }

    static char path[RD_PATH_MAX];
    snprintf(path, sizeof(path), RD_LIST_DIR "/%s", name_of(0));
    ESP_LOGI(TAG, "打开：%s", path);

    const int64_t t0 = esp_timer_get_time();
    if (!rd_book_open(path, NULL)) {
        ESP_LOGE(TAG, "打不开 —— 见上面的错误行");
        ESP_LOGI(TAG, "=== 自检结束 ===");
        return;
    }
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "《%s》[%s] %s %d 字节 %d 页（分页耗时 %dms）",
             rd_book_title(), rd_book_kind(), rd_book_encoding(),
             rd_book_bytes(), rd_book_pages(), (int)ms);

    static rd_page_t pg;
    if (rd_book_load_page(&pg)) {
        ESP_LOGI(TAG, "第 1 页 %d 行，首行/末行：", pg.lines);
        if (pg.lines > 0) {
            log_cps(pg.cp[0], pg.n[0], 24);
            log_cps(pg.cp[pg.lines - 1], pg.n[pg.lines - 1], 24);
        }
    } else {
        ESP_LOGE(TAG, "取第 1 页失败");
    }

    if (rd_book_goto(rd_book_pages() - 1) && rd_book_load_page(&pg)) {
        ESP_LOGI(TAG, "末页（第 %d 页）%d 行，末行：", rd_book_pages(), pg.lines);
        if (pg.lines > 0) log_cps(pg.cp[pg.lines - 1], pg.n[pg.lines - 1], 24);
    }

    rd_book_close();

    ESP_LOGI(TAG, "--- 字库覆盖率抽样（每本前 30 页）---");
    for (int f = 0; f < s_n && f < 5; f++) {
        static char p2[RD_PATH_MAX];
        snprintf(p2, sizeof(p2), RD_LIST_DIR "/%s", name_of(f));
        if (!rd_book_open(p2, NULL)) continue;

        const int scan = rd_book_pages() < 30 ? rd_book_pages() : 30;
        int total = 0, miss = 0;
        uint32_t seen[16];
        int nseen = 0;

        for (int pg = 0; pg < scan; pg++) {
            rd_page_t lpg;
            if (!rd_book_goto(pg) || !rd_book_load_page(&lpg)) continue;
            for (int L = 0; L < lpg.lines; L++) {
                for (int k = 0; k < lpg.n[L]; k++) {
                    const uint32_t c = lpg.cp[L][k];
                    total++;
                    if (rd_font_has(c)) continue;
                    miss++;
                    if (nseen < 16) {
                        bool dup = false;
                        for (int q = 0; q < nseen; q++) if (seen[q] == c) dup = true;
                        if (!dup) seen[nseen++] = c;
                    }
                }
            }
        }
        static char u8[RD_NAME_U8_MAX];
        rd_name_utf8(name_of(f), u8, (int)sizeof(u8));
        ESP_LOGI(TAG, "  %s：抽样 %d 字，缺 %d（%.2f%%）", u8, total, miss,
                 total ? miss * 100.0 / total : 0.0);
        if (nseen) {
            ESP_LOGI(TAG, "     缺字样例（U+码点）：");
            for (int q = 0; q < nseen; q++) {
                char tmp[8];
                const uint32_t c = seen[q];
                int m = 0;
                if (c < 0x800) { tmp[m++] = (char)(0xC0 | (c >> 6)); tmp[m++] = (char)(0x80 | (c & 0x3F)); }
                else { tmp[m++] = (char)(0xE0 | (c >> 12)); tmp[m++] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[m++] = (char)(0x80 | (c & 0x3F)); }
                tmp[m] = 0;
                ESP_LOGI(TAG, "       U+%04X %s", (unsigned)c, tmp);
            }
        }
        rd_book_close();
    }

    net_wifi_mem("阅读自检后");
    ESP_LOGI(TAG, "=== 自检结束 ===");
}
#endif
