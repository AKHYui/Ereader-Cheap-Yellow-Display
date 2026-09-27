#include "pv_app.h"
#include "pv_config.h"
#include "pv_disp.h"
#include "nui_sleep.h"
#include "pv_scan.h"
#include "pv_touch.h"
#include "pv_jpeg.h"
#include "pv_bmp.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nui_ui.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = PV_TAG;

#if PV_BUILTIN_CARD

extern const uint8_t _binary_pv_card_1080p_jpg_start[];
extern const uint8_t _binary_pv_card_1080p_jpg_end[];
#endif

static int  s_btn_active;
static int  s_press_seen;
static int  s_total;
static int  s_nsd;
static int  s_ntest;
static int  s_ncard;
static int  s_off;

static int bar_hit(int x, int y, void *ctx)
{
    (void)ctx;
    int bx = 0, by = 0, bw = 0, bh = 0;

    pv_disp_home_rect(&bx, &by, &bw, &bh);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
        return PV_PRESS_HOME;
    }

    pv_disp_del_rect(&bx, &by, &bw, &bh);
    if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
        return PV_PRESS_DEL;
    }

    for (int which = -1; which <= 1; which += 2) {
        pv_disp_btn_rect(which, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            return (which < 0) ? PV_PRESS_LEFT : PV_PRESS_RIGHT;
        }
    }
    return PV_PRESS_NONE;
}

#if PV_TOUCH_LOG

static void touch_log(const pv_touch_evt_t *ev, int hit_evt, int hit_st, int idx)
{
    char line[160];
    snprintf(line, sizeof(line),
             "%s raw=(%4d,%4d) z=%5d map=(%3d,%3d) hitEvt=%2d hitStable=%2d idx=%d",
             ev->down ? "DOWN" : "UP  ", ev->rx, ev->ry, ev->z,
             ev->x, ev->y, hit_evt, hit_st, idx);
    ESP_LOGI(TAG, "touch %s", line);

    FILE *f = fopen(PV_TOUCH_LOG_PATH, "a");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
}

static void touch_log_dump(void)
{
    FILE *f = fopen(PV_TOUCH_LOG_PATH, "r");
    if (!f) return;

    ESP_LOGW(TAG, "=== 上次运行的触摸记录（%s）===", PV_TOUCH_LOG_PATH);
    char line[192];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < 200) {
        size_t k = strlen(line);
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = 0;
        if (k) {
            ESP_LOGW(TAG, "  t %s", line);
            n++;
        }
    }
    fclose(f);
    ESP_LOGW(TAG, "=== 共 %d 行，记录结束 ===", n);

    f = fopen(PV_TOUCH_LOG_PATH, "w");
    if (f) fclose(f);
}
#else

#define touch_log(ev, a, b, c)   do { (void)(ev); (void)(a); (void)(b); (void)(c); } while (0)
#define touch_log_dump()         do { } while (0)
#endif

#if PV_DEL_LOG

static void del_log(const char *line)
{
    ESP_LOGW(TAG, "del %s", line);

    FILE *f = fopen(PV_DEL_LOG_PATH, "a");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
}

static void del_logf(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    del_log(line);
}

static void del_log_dump(void)
{
    FILE *f = fopen(PV_DEL_LOG_PATH, "r");
    if (!f) return;

    ESP_LOGW(TAG, "=== 上次的删除记录（%s）===", PV_DEL_LOG_PATH);
    char line[224];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < 100) {
        size_t k = strlen(line);
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = 0;
        if (k) {
            ESP_LOGW(TAG, "  d %s", line);
            n++;
        }
    }
    fclose(f);
    ESP_LOGW(TAG, "=== 共 %d 行，记录结束 ===", n);

    f = fopen(PV_DEL_LOG_PATH, "w");
    if (f) fclose(f);
}
#else
#define del_log(line)     do { (void)(line); } while (0)
#define del_logf(...)     do { } while (0)
#define del_log_dump()    do { } while (0)
#endif

#define CF_BTN_W     100
#define CF_BTN_H      56
#define CF_BTN_Y     196
#define CF_CANCEL_X   14
#define CF_DEL_X     (CF_CANCEL_X + CF_BTN_W + 12)

enum { CF_NONE = 0, CF_CANCEL = 1, CF_DEL = 2 };

#define CF_HIT_PAD   6

static int confirm_hit(int x, int y, void *ctx)
{
    (void)ctx;

    if (y < CF_BTN_Y - CF_HIT_PAD || y >= CF_BTN_Y + CF_BTN_H + CF_HIT_PAD) {
        return CF_NONE;
    }
    if (x >= CF_CANCEL_X - CF_HIT_PAD &&
        x <  CF_CANCEL_X + CF_BTN_W + CF_HIT_PAD) return CF_CANCEL;
    if (x >= CF_DEL_X - CF_HIT_PAD &&
        x <  CF_DEL_X + CF_BTN_W + CF_HIT_PAD)    return CF_DEL;
    return CF_NONE;
}

static void confirm_draw(int idx, int total, int pressed)
{
    char num[16];
    snprintf(num, sizeof(num), "%d / %d", idx + 1, total);

    pv_disp_page_begin(NUI_BG);

    for (int sy = 0; sy < PV_SCR_H; sy++) {
        uint16_t *row = pv_disp_page_row(sy);
        if (!row) continue;

        nui_text_mid_center(row, sy, PV_SCR_W / 2, 88, 48,
                            "确定删除", NUI_WARN, NUI_BG);

        {
            const int w = nui_ascii2x_w(num);
            nui_ascii2x_row(row, sy, (PV_SCR_W - w) / 2, 144, num, NUI_FG);
        }

        nui_button_row(row, sy, CF_CANCEL_X, CF_BTN_Y, CF_BTN_W, CF_BTN_H,
                       "取消", pressed == CF_CANCEL);

        {
            const uint16_t bg = (pressed == CF_DEL) ? NUI_PRESS : NUI_ERR;
            nui_frame_row(row, sy, CF_DEL_X, CF_BTN_Y, CF_BTN_W, CF_BTN_H,
                          bg, NUI_EDGE, 2);
            nui_text_mid_center(row, sy, CF_DEL_X + CF_BTN_W / 2, CF_BTN_Y,
                                CF_BTN_H, "删除", NUI_FG, bg);
        }
    }

    pv_disp_page_end();
}

static bool confirm_delete(int idx, int total)
{
    int pressed = 0;
    confirm_draw(idx, total, pressed);
    del_logf("确认页打开: 第 %d/%d 项", idx + 1, total);

    for (;;) {
        if (pv_touch_is_down()) {
            const int h = pv_touch_hit(confirm_hit, NULL);
            if (h != pressed) {
                pressed = h;
                confirm_draw(idx, total, pressed);
            }
        }

        pv_touch_evt_t ev;
        if (!pv_touch_take(&ev)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ev.down) continue;

        const int h = pv_touch_hit(confirm_hit, NULL);
        const int was = pressed;
        pressed = 0;

        if (h == CF_DEL) {
            del_log("确认页结算: 点了[删除] -> 执行删除");
            return true;
        }

        del_logf("确认页结算: 按下期间命中=%d 抬起命中=%d -> %s",
                 was, h, (h == CF_CANCEL) ? "取消(点了取消键)" : "取消(点空白)");
        return false;
    }
}

static bool delete_file(int idx)
{

    static char path[PV_PATH_MAX];
    pv_scan_path(idx - s_off, path, sizeof(path));

    if (!path[0]) {

        del_logf("删除中止: 索引越界 idx=%d s_off=%d s_nsd=%d -> 路径为空",
                 idx, s_off, s_nsd);
        return false;
    }

    errno = 0;
    const int r = remove(path);
    const int e = errno;

    struct stat st;
    const int still = (stat(path, &st) == 0);

    del_logf("remove(%s) -> %d errno=%d(%s) 复核: 文件%s",
             path, r, e, strerror(e), still ? "仍然存在 ✗" : "已消失 ✓");

    if (r != 0 || still) return false;

    ESP_LOGW(TAG, "已删除 %s", path);
    return true;
}

static void show_index(int idx)
{
    pv_img_err_t e = PV_IMG_OK;

    if (s_ntest && idx == 0) {
        ESP_LOGI(TAG, "--- [%d/%d] 色彩自检页（上半 本机序 / 下半 字节对调）---",
                 idx + 1, s_total);
        pv_disp_colortest();
    } else if (s_ncard && idx == s_ntest) {
#if PV_BUILTIN_CARD
        const unsigned len = (unsigned)(_binary_pv_card_1080p_jpg_end -
                                        _binary_pv_card_1080p_jpg_start);
        ESP_LOGI(TAG, "--- [%d/%d] 内置测试卡 1920x1080 baseline，%u 字节（对照组）---",
                 idx + 1, s_total, len);
        e = pv_jpeg_show_ram(_binary_pv_card_1080p_jpg_start, len);
#else
        e = PV_IMG_ERR_OPEN;
#endif
    } else {
        const int sd = idx - s_off;
        static char path[PV_PATH_MAX];
        pv_scan_path(sd, path, sizeof(path));
        ESP_LOGI(TAG, "--- [%d/%d] SD %d/%d: %s ---",
                 idx + 1, s_total, sd + 1, s_nsd, pv_scan_name(sd));
        if (!path[0]) {

            ESP_LOGE(TAG, "第 %d 项拼不出有效路径，放弃显示", idx + 1);
            e = PV_IMG_ERR_OPEN;
        } else {
            e = pv_scan_is_jpeg(sd) ? pv_jpeg_show(path) : pv_bmp_show(path);
        }
    }

    if (e != PV_IMG_OK) {
        ESP_LOGW(TAG, "显示失败：%s", pv_img_err_text(e));
        pv_disp_img_clear();
    }

    pv_disp_bar(idx + 1, s_total, s_btn_active);
}

void pv_app_run(void)
{
    ESP_LOGI(TAG, "=== 进入图片查看器 ===");

    pv_disp_img_clear();
    pv_disp_bar(0, 0, PV_PRESS_NONE);

    s_nsd   = pv_scan_run();
    s_ntest = PV_COLORTEST ? 1 : 0;

    s_ncard = (PV_BUILTIN_CARD && (PV_CARD_FIRST || s_nsd == 0)) ? 1 : 0;
    s_off   = s_ntest + s_ncard;
    s_total = s_off + s_nsd;

    touch_log_dump();
    del_log_dump();

    {
        int bx = 0, by = 0, bw = 0, bh = 0;
        pv_disp_btn_rect(-1, &bx, &by, &bw, &bh);
        ESP_LOGI(TAG, "左按钮热区 x=%d..%d y=%d..%d", bx, bx + bw - 1, by, by + bh - 1);
        pv_disp_btn_rect(1, &bx, &by, &bw, &bh);
        ESP_LOGI(TAG, "右按钮热区 x=%d..%d y=%d..%d", bx, bx + bw - 1, by, by + bh - 1);
        pv_disp_home_rect(&bx, &by, &bw, &bh);
        ESP_LOGI(TAG, "主页键热区 x=%d..%d y=%d..%d（按它回主菜单）",
                 bx, bx + bw - 1, by, by + bh - 1);
    }

    if (s_total <= 0) {
        ESP_LOGW(TAG, "%s 下没有找到 jpg / bmp 文件，且内置测试卡已关闭", PV_DIR);
        return;
    }

    ESP_LOGI(TAG, "列表：自检页 %d + 测试卡 %d + SD 图 %d = 共 %d 项，"
                  "字节序=%s，轮播 %dms",
             s_ntest, s_ncard, s_nsd, s_total,
             pv_disp_get_swap() ? "对调" : "本机", (int)PV_AUTOPLAY_MS);

    int idx = 0;
    show_index(idx);

#if PV_AUTOPLAY_MS > 0

    uint32_t last_auto = xTaskGetTickCount();
#endif

    for (;;) {
        bool go_home = false;

        if (nui_sleep_poll()) {
            if (nui_sleep_test_should_back()) return;
            show_index(idx);
            continue;
        }

        if (pv_touch_is_down()) {
            int h = pv_touch_hit(bar_hit, NULL);

            if (h == PV_PRESS_DEL && idx < s_off) h = PV_PRESS_DEL_BAD;
            if (h != s_btn_active) {
                s_btn_active = h;
                pv_disp_bar(idx + 1, s_total, h);
            }
        }

        pv_touch_evt_t ev;
        while (pv_touch_take(&ev)) {

            const int hit_evt = bar_hit(ev.x, ev.y, NULL);
            const int hit_st  = pv_touch_hit(bar_hit, NULL);

            if (ev.down) {
                s_press_seen = 1;
            } else {
                const int was = s_btn_active;
                s_btn_active = PV_PRESS_NONE;

                if (s_press_seen) {
                    s_press_seen = 0;

                    if (hit_st == PV_PRESS_HOME) {
                        go_home = true;
                    } else if (hit_st == PV_PRESS_DEL) {

                        del_logf("按下删除键: idx=%d s_off=%d s_nsd=%d s_total=%d",
                                 idx, s_off, s_nsd, s_total);
                        if (idx < s_off) {
                            ESP_LOGW(TAG, "第 %d 项是自检页/内置测试卡，不是文件，删不了",
                                     idx + 1);
                            del_logf("忽略: idx=%d 落在自检/测试卡段（s_off=%d），不是文件",
                                     idx, s_off);
                        } else if (confirm_delete(idx, s_total)) {
                            if (delete_file(idx)) {
                                s_nsd   = pv_scan_run();
                                s_total = s_off + s_nsd;
                                del_logf("重扫完成: s_nsd=%d s_total=%d idx=%d",
                                         s_nsd, s_total, idx);
                                if (s_total <= 0) {
                                    ESP_LOGW(TAG, "SD 卡上已经没有图片，退出查看器");
                                    del_log("没有图片了 -> 退出查看器回主菜单");
                                    pv_disp_img_clear();
                                    return;
                                }
                                if (idx >= s_total) idx = s_total - 1;
                                show_index(idx);
                            } else {
                                del_log("删除未成功 -> 把原图放回去（详见上一行 remove 结果）");
                                show_index(idx);
                            }
                        } else {
                            del_log("用户取消 -> 把原图放回去");
                            show_index(idx);
                        }
                    } else if (hit_st == PV_PRESS_LEFT ||
                               hit_st == PV_PRESS_RIGHT) {
                        idx = (hit_st == PV_PRESS_LEFT)
                                  ? ((idx + s_total - 1) % s_total)
                                  : ((idx + 1) % s_total);
                        show_index(idx);
#if PV_AUTOPLAY_MS > 0
                        last_auto = xTaskGetTickCount();
#endif
                    } else if (was) {
                        pv_disp_bar(idx + 1, s_total, PV_PRESS_NONE);
                    }
                }
            }

            touch_log(&ev, hit_evt, hit_st, idx);
        }

        if (go_home) {
            ESP_LOGI(TAG, "按了主页键 -> 回主菜单");
            return;
        }

#if PV_AUTOPLAY_MS > 0

        if (s_total > 1 &&
            (xTaskGetTickCount() - last_auto) * portTICK_PERIOD_MS
                >= (uint32_t)PV_AUTOPLAY_MS) {
            idx = (idx + 1) % s_total;
            show_index(idx);
            last_auto = xTaskGetTickCount();
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
