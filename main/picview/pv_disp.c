#include "pv_disp.h"
#include "pv_config.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = PV_TAG;

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t         s_done;

static uint16_t (*s_band)[PV_SCR_W];

static int s_y0;
static int s_y_limit;

static int s_img_ox;
static int s_img_oy;
static int s_img_tw;
static int s_img_th;
static int s_src_w;
static int s_src_h;

static int s_seen_w;
static int s_seen_h;

static bool s_swap = (PV_RGB565_SWAP != 0);

static int s_swap_force = -1;

void pv_disp_set_swap(bool on) { s_swap = on; }
bool pv_disp_get_swap(void)    { return s_swap; }

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;

    BaseType_t hp = pdFALSE;
    if (s_done) {
        xSemaphoreGiveFromISR(s_done, &hp);
    }
    return hp == pdTRUE;
}

static void flush_band(int h)
{
    if (h <= 0) return;

    const bool swap = (s_swap_force >= 0) ? (s_swap_force != 0) : s_swap;
    if (swap) {
        uint16_t *p = (uint16_t *)s_band;
        const size_t n = (size_t)h * PV_SCR_W;
        for (size_t i = 0; i < n; i++) {
            p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
        }
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, s_y0,
                                              PV_SCR_W, s_y0 + h, s_band));
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static void band_fill(uint16_t color)
{
    const size_t n = (size_t)PV_BAND_H * PV_SCR_W;
    if (color == 0) {
        memset(s_band, 0, n * sizeof(uint16_t));
        return;
    }
    uint16_t *p = (uint16_t *)s_band;
    for (size_t i = 0; i < n; i++) {
        p[i] = color;
    }
}

static void band_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int x1 = x + w;
    if (x1 > PV_SCR_W) x1 = PV_SCR_W;
    if (x1 <= x0) return;

    for (int r = 0; r < h; r++) {
        const int yy = y + r;
        if (yy < 0 || yy >= PV_BAND_H) continue;
        uint16_t *row = s_band[yy];
        for (int i = x0; i < x1; i++) {
            row[i] = color;
        }
    }
}

esp_err_t pv_disp_init(esp_lcd_panel_handle_t panel,
                       esp_lcd_panel_io_handle_t io)
{
    s_panel = panel;
    s_io    = io;

    s_done = xSemaphoreCreateBinary();
    if (!s_done) {
        ESP_LOGE(TAG, "创建传输完成信号量失败");
        return ESP_ERR_NO_MEM;
    }

    const size_t band_bytes = (size_t)PV_BAND_H * PV_SCR_W * sizeof(uint16_t);
    s_band = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_band) {
        ESP_LOGE(TAG, "行带分配失败：需要 %u 字节，DMA 最大可分配块只有 %u",
                 (unsigned)band_bytes,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        vSemaphoreDelete(s_done);
        s_done = NULL;
        return ESP_ERR_NO_MEM;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    ESP_LOGI(TAG, "显示层就绪：行带 %dx%d (%u 字节 DMA)，最大可分配块 %u",
             PV_SCR_W, PV_BAND_H, (unsigned)band_bytes,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;
}

void pv_disp_deinit(void)
{
    if (s_band) {
        heap_caps_free(s_band);
        s_band = NULL;
    }
    if (s_done) {
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
}

static uint16_t s_acc_r[PV_SCR_W];
static uint16_t s_acc_g[PV_SCR_W];
static uint16_t s_acc_b[PV_SCR_W];
static uint8_t  s_acc_n[PV_SCR_W];
static uint16_t s_last_row[PV_SCR_W];
static int      s_acc_dy = -1;

static void acc_clear(void)
{

    memset(s_acc_n, 0, sizeof(s_acc_n));
    memset(s_acc_r, 0, sizeof(s_acc_r));
    memset(s_acc_g, 0, sizeof(s_acc_g));
    memset(s_acc_b, 0, sizeof(s_acc_b));
}

#if PV_STATS

static uint32_t s_out_r, s_out_g, s_out_b, s_out_n;

static void stat_row(const uint16_t *row, int n)
{
    for (int i = 0; i < n; i++) {
        const uint16_t v = row[i];
        s_out_r += (v >> 11) & 0x1Fu;
        s_out_g += (v >> 5)  & 0x3Fu;
        s_out_b +=  v         & 0x1Fu;
    }
    s_out_n += (uint32_t)n;
}

static void stat_reset(void)
{
    s_out_r = s_out_g = s_out_b = s_out_n = 0;
}
#endif

static void acc_settle(void)
{
    uint16_t carry = 0;
    bool     have  = false;

    for (int dx = 0; dx < s_img_tw; dx++) {
        uint16_t v;
        if (s_acc_n[dx]) {
            const uint32_t n = s_acc_n[dx];
            const uint32_t r = (s_acc_r[dx] + n / 2) / n;
            const uint32_t g = (s_acc_g[dx] + n / 2) / n;
            const uint32_t b = (s_acc_b[dx] + n / 2) / n;
            v = (uint16_t)(((r & 0x1Fu) << 11) | ((g & 0x3Fu) << 5) | (b & 0x1Fu));
            have = true;
        } else {

            v = have ? carry : 0;
        }
        carry      = v;
        s_last_row[dx] = v;
    }
}

static void acc_push_row(int dy)
{
    const int py = dy + s_img_oy;
    while (py >= s_y0 + PV_BAND_H) {
        flush_band(PV_BAND_H);
        s_y0 += PV_BAND_H;
        band_fill(PV_BG);
    }
    if (py < s_y0) return;

    uint16_t *dst = s_band[py - s_y0];
    memcpy(&dst[s_img_ox], s_last_row, (size_t)s_img_tw * sizeof(uint16_t));
#if PV_STATS
    stat_row(s_last_row, s_img_tw);
#endif
}

void pv_disp_img_begin(int src_w, int src_h, int dst_w, int dst_h)
{
    if (src_w < 1) src_w = 1;
    if (src_h < 1) src_h = 1;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (dst_w > PV_SCR_W) dst_w = PV_SCR_W;
    if (dst_h > PV_IMG_H) dst_h = PV_IMG_H;

    s_src_w  = src_w;
    s_src_h  = src_h;
    s_seen_w = 0;
    s_seen_h = 0;
    s_img_tw = dst_w;
    s_img_th = dst_h;
    s_img_ox = (PV_SCR_W - dst_w) / 2;
    s_img_oy = (PV_IMG_H - dst_h) / 2;

    s_y0      = 0;
    s_y_limit = PV_IMG_H;

    s_acc_dy = -1;
    acc_clear();
#if PV_STATS
    stat_reset();
#endif

    band_fill(PV_BG);

#if PV_STATS
    ESP_LOGI(TAG, "画布 src %dx%d -> dst %dx%d 偏移(%d,%d)%s",
             src_w, src_h, dst_w, dst_h, s_img_ox, s_img_oy,
             (src_w == dst_w && src_h == dst_h) ? " [直通]" : " [抽取]");
#endif
}

void pv_disp_img_block(int x, int y, int w, int h,
                       const uint16_t *px, int stride)
{
    if (!s_band || !px || w <= 0 || h <= 0 || stride <= 0) return;

    if (x + w > s_seen_w) s_seen_w = x + w;
    if (y + h > s_seen_h) s_seen_h = y + h;

    const bool identity = (s_src_w == s_img_tw && s_src_h == s_img_th);

    if (identity) {
        for (int r = 0; r < h; r++) {
            const int sy = y + r;
            if (sy < 0 || sy >= s_src_h) continue;

            const int py = sy + s_img_oy;
            while (py >= s_y0 + PV_BAND_H) {
                flush_band(PV_BAND_H);
                s_y0 += PV_BAND_H;
                band_fill(PV_BG);
            }
            if (py < s_y0) continue;

            const uint16_t *src = px + (size_t)r * (size_t)stride;
            uint16_t       *dst = s_band[py - s_y0];

            int cx = x;
            int cw = w;
            if (cx < 0) {
                src -= cx;
                cw  += cx;
                cx   = 0;
            }
            if (cx + cw > s_src_w) cw = s_src_w - cx;
            if (cw <= 0) continue;

            memcpy(&dst[s_img_ox + cx], src, (size_t)cw * sizeof(uint16_t));
#if PV_STATS
            stat_row(&dst[s_img_ox + cx], cw);
#endif
        }
        return;
    }

    for (int r = 0; r < h; r++) {
        const int sy = y + r;
        if (sy < 0 || sy >= s_src_h) continue;

        const int dy = (int)((int32_t)sy * s_img_th / s_src_h);
        if (dy < 0 || dy >= s_img_th) continue;

        if (dy != s_acc_dy) {
            if (s_acc_dy >= 0) {
                acc_settle();
                acc_push_row(s_acc_dy);

                for (int g = s_acc_dy + 1; g < dy; g++) {
                    acc_push_row(g);
                }
            }
            acc_clear();
            s_acc_dy = dy;
        }

        const uint16_t *src = px + (size_t)r * (size_t)stride;

        for (int c = 0; c < w; c++) {
            const int sx = x + c;
            if (sx < 0 || sx >= s_src_w) continue;

            const int dx = (int)((int32_t)sx * s_img_tw / s_src_w);
            if (dx < 0 || dx >= s_img_tw) continue;

            const uint16_t v = src[c];
            s_acc_r[dx] += (v >> 11) & 0x1Fu;
            s_acc_g[dx] += (v >> 5)  & 0x3Fu;
            s_acc_b[dx] +=  v         & 0x1Fu;
            if (s_acc_n[dx] < 0xFFu) s_acc_n[dx]++;
        }
    }
}

void pv_disp_img_end(void)
{
#if PV_STATS

    if (s_seen_w != s_src_w || s_seen_h != s_src_h) {
        ESP_LOGW(TAG, "解码输出与预测不符：预测 %dx%d，实际 %dx%d "
                      "-> 抽取/居中会用错坐标系（图会整体偏移、偏小）",
                 s_src_w, s_src_h, s_seen_w, s_seen_h);
    }

    if (s_out_n) {
        ESP_LOGI(TAG, "输出均值x10 R=%lu G=%lu B=%lu (%lu 像素)",
                 (unsigned long)((uint64_t)s_out_r * 10 / s_out_n),
                 (unsigned long)((uint64_t)s_out_g * 10 / s_out_n),
                 (unsigned long)((uint64_t)s_out_b * 10 / s_out_n),
                 (unsigned long)s_out_n);
    }
#endif

    if (s_acc_dy >= 0) {
        acc_settle();
        acc_push_row(s_acc_dy);
        s_acc_dy = -1;
    }

    while (s_y0 < s_y_limit) {
        int h = s_y_limit - s_y0;
        if (h > PV_BAND_H) h = PV_BAND_H;
        flush_band(h);
        s_y0 += h;
        band_fill(PV_BG);
    }
}

void pv_disp_img_clear(void)
{
    if (!s_band) return;

    s_y0      = 0;
    s_y_limit = PV_IMG_H;
    band_fill(PV_BG);

    while (s_y0 < PV_IMG_H) {
        int h = PV_IMG_H - s_y0;
        if (h > PV_BAND_H) h = PV_BAND_H;
        flush_band(h);
        s_y0 += h;
        band_fill(PV_BG);
    }
}

static uint16_t s_page_bg;

#if PV_STATS

static uint32_t s_pg_r, s_pg_g, s_pg_b, s_pg_n;

static void page_stat(int rows)
{
    for (int r = 0; r < rows; r++) {
        const uint16_t *row = s_band[r];
        for (int x = 0; x < PV_SCR_W; x++) {
            const uint16_t v = row[x];
            s_pg_r += (v >> 11) & 0x1Fu;
            s_pg_g += (v >> 5)  & 0x3Fu;
            s_pg_b +=  v         & 0x1Fu;
        }
    }
    s_pg_n += (uint32_t)rows * PV_SCR_W;
}
#else
#define page_stat(rows)   do { (void)(rows); } while (0)
#endif

void pv_disp_page_begin(uint16_t bg)
{
    s_page_bg = bg;
    s_y0      = 0;
    s_y_limit = PV_SCR_H;
    band_fill(bg);
#if PV_STATS
    s_pg_r = s_pg_g = s_pg_b = s_pg_n = 0;
#endif
}

uint16_t *pv_disp_page_row(int y)
{
    if (!s_band || y < 0 || y >= PV_SCR_H) return NULL;

    while (y >= s_y0 + PV_BAND_H) {
        page_stat(PV_BAND_H);
        flush_band(PV_BAND_H);
        s_y0 += PV_BAND_H;
        band_fill(s_page_bg);
    }
    if (y < s_y0) return NULL;
    return s_band[y - s_y0];
}

void pv_disp_page_end(void)
{
    while (s_y0 < PV_SCR_H) {
        int h = PV_SCR_H - s_y0;
        if (h > PV_BAND_H) h = PV_BAND_H;
        page_stat(h);
        flush_band(h);
        s_y0 += h;
        band_fill(s_page_bg);
    }

#if PV_STATS
    if (s_pg_n) {
        ESP_LOGI(TAG, "页面输出均值x10 R=%lu G=%lu B=%lu (%lu 像素)",
                 (unsigned long)((uint64_t)s_pg_r * 10 / s_pg_n),
                 (unsigned long)((uint64_t)s_pg_g * 10 / s_pg_n),
                 (unsigned long)((uint64_t)s_pg_b * 10 / s_pg_n),
                 (unsigned long)s_pg_n);
    }
#endif
}

static void page_fill_rows(int y, int h, uint16_t color)
{
    for (int r = 0; r < h; r++) {
        uint16_t *row = pv_disp_page_row(y + r);
        if (!row) continue;
        for (int x = 0; x < PV_SCR_W; x++) row[x] = color;
    }
}

static void page_flush_to(int y)
{
    while (s_y0 < y) {
        int h = y - s_y0;
        if (h > PV_BAND_H) h = PV_BAND_H;
        flush_band(h);
        s_y0 += h;
        band_fill(s_page_bg);
    }
}

void pv_disp_colortest(void)
{
    if (!s_band) return;

    static const uint16_t cols[5] = {
        0xF800,
        0x07E0,
        0x001F,
        0x18E3,
        0x39E7,
    };

    s_y0      = 0;
    s_y_limit = PV_IMG_H;
    pv_disp_page_begin(PV_BG);

    s_swap_force = 0;
    for (int i = 0; i < 5; i++) {
        page_fill_rows(i * 24, 24, cols[i]);
    }
    page_flush_to(120);

    page_fill_rows(120, 4, 0xFFFF);
    page_flush_to(124);

    s_swap_force = 1;
    for (int i = 0; i < 5; i++) {
        page_fill_rows(124 + i * 24, 24, cols[i]);
    }
    page_flush_to(PV_IMG_H);

    s_swap_force = -1;

    ESP_LOGI(TAG, "=== 色彩自检页 ===");
    ESP_LOGI(TAG, "上半区=本机序，下半区=字节对调，中间一条白线");
    ESP_LOGI(TAG, "每半区 5 条，从上到下：纯红 / 纯绿 / 纯蓝 / 控制条底色 / 按钮底色");
    ESP_LOGI(TAG, "判读：哪半区的第 1~3 条是【红/绿/蓝】，那半区就是对的字节序；");
    ESP_LOGI(TAG, "      对的那半区第 4 条应是深灰（不是紫）、第 5 条略亮的深灰");
    ESP_LOGI(TAG, "当前 PV_RGB565_SWAP = %d（正确时下半区对、上半区全是错色）",
             (int)(PV_RGB565_SWAP != 0));
}

static const uint8_t SEG_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

#define SEG_W   10
#define SEG_H   18
#define SEG_T    3

#define SEG_ADV (SEG_W + 3)

static void seg_digit(int x, int y, int d, uint16_t color)
{
    if (d < 0 || d > 9) return;

    const uint8_t s    = SEG_DIGIT[d];
    const int     half = SEG_H / 2;
    const int     vlen = half - SEG_T;
    const int     mid  = half - SEG_T / 2;

    if (s & 0x01) band_rect(x + SEG_T, y,                   SEG_W - 2 * SEG_T, SEG_T, color);
    if (s & 0x02) band_rect(x + SEG_W - SEG_T, y + SEG_T,   SEG_T, vlen, color);
    if (s & 0x04) band_rect(x + SEG_W - SEG_T, y + half,    SEG_T, vlen, color);
    if (s & 0x08) band_rect(x + SEG_T, y + SEG_H - SEG_T,   SEG_W - 2 * SEG_T, SEG_T, color);
    if (s & 0x10) band_rect(x, y + half,                    SEG_T, vlen, color);
    if (s & 0x20) band_rect(x, y + SEG_T,                   SEG_T, vlen, color);
    if (s & 0x40) band_rect(x + SEG_T, y + mid,             SEG_W - 2 * SEG_T, SEG_T, color);
}

static void seg_slash(int x, int y, uint16_t color)
{
    const int span = SEG_W - 2;
    for (int i = 0; i < SEG_H; i++) {
        const int px = x + span - (i * span) / SEG_H;
        band_rect(px, y + i, 2, 1, color);
    }
}

static void bar_layout(int *x0, int *x1, int *x2, int *x3, int *by)
{
    const int y = (PV_BAR_H - PV_BTN_H) / 2;
    *x0 = PV_BTN_MARGIN;
    *x1 = *x0 + PV_BTN_W  + PV_BTN_MARGIN;
    *x2 = *x1 + PV_HOME_W + PV_BTN_MARGIN;
    *x3 = *x2 + PV_DEL_W  + PV_BTN_MARGIN;
    *by = y;
}

static void home_layout(int *x, int *y, int *w, int *h)
{
    int x0, x1, x2, x3, by;
    bar_layout(&x0, &x1, &x2, &x3, &by);
    *x = x1;
    *y = PV_IMG_H + by;
    *w = PV_HOME_W;
    *h = PV_HOME_H;
}

static void del_layout(int *x, int *y, int *w, int *h)
{
    int x0, x1, x2, x3, by;
    bar_layout(&x0, &x1, &x2, &x3, &by);
    *x = x2;
    *y = PV_IMG_H + by;
    *w = PV_DEL_W;
    *h = PV_BTN_H;
}

static void btn_draw(int x, int y, bool pressed, bool point_right)
{
    band_rect(x, y, PV_BTN_W, PV_BTN_H, pressed ? PV_BTN_PRESS : PV_BTN_BG);

    const int aw = 12;
    const int ah = 16;
    const int ax = x + (PV_BTN_W - aw) / 2;
    const int ay = y + (PV_BTN_H - ah) / 2;

    for (int i = 0; i < ah; i++) {
        const int d   = (i < ah / 2) ? i : (ah - 1 - i);
        const int run = d + 1;
        const int sx  = point_right ? ax : (ax + aw - run);
        band_rect(sx, ay + i, run, 1, PV_BTN_FG);
    }
}

#define TRASH_W  18
#define TRASH_H  22

static void trash_icon(int x, int y, uint16_t c)
{
    band_rect(x + 6,  y,      6,  3, c);
    band_rect(x,      y + 3,  18, 3, c);
    band_rect(x + 2,  y + 6,  3, 13, c);
    band_rect(x + 15, y + 6,  3, 13, c);
    band_rect(x + 2,  y + 20, 15, 2, c);
    band_rect(x + 9,  y + 9,  2,  9, c);
}

static int fmt_idx(char *buf, int idx, int total)
{
    int n = 0;
    if (idx < 0)   idx = 0;
    if (total < 0) total = 0;

    if (idx >= 100) buf[n++] = (char)('0' + (idx / 100) % 10);
    if (idx >= 10)  buf[n++] = (char)('0' + (idx / 10) % 10);
    buf[n++] = (char)('0' + idx % 10);

    buf[n++] = '/';

    if (total >= 100) buf[n++] = (char)('0' + (total / 100) % 10);
    if (total >= 10)  buf[n++] = (char)('0' + (total / 10) % 10);
    buf[n++] = (char)('0' + total % 10);

    return n;
}

void pv_disp_bar(int idx, int total, int press)
{
    if (!s_band) return;

    int x0, x1, x2, x3, by;
    bar_layout(&x0, &x1, &x2, &x3, &by);

    s_y0      = PV_IMG_H;
    s_y_limit = PV_SCR_H;
    band_fill(PV_BAR_BG);

    btn_draw(x0, by, press == PV_PRESS_LEFT,  false);
    btn_draw(x3, by, press == PV_PRESS_RIGHT, true);

    band_rect(x1, by, PV_HOME_W, PV_HOME_H,
              (press == PV_PRESS_HOME) ? PV_BTN_PRESS : PV_BTN_BG);

    {
        const uint16_t bg = (press == PV_PRESS_DEL)     ? PV_BTN_PRESS
                          : (press == PV_PRESS_DEL_BAD) ? PV_BAD
                                                        : PV_BTN_BG;
        band_rect(x2, by, PV_DEL_W, PV_BTN_H, bg);
        trash_icon(x2 + (PV_DEL_W - TRASH_W) / 2,
                   by + (PV_BTN_H - TRASH_H) / 2, PV_BTN_FG);
    }

    char txt[12];
    const int n = fmt_idx(txt, idx, total);

    const int total_w = n * SEG_ADV - 3;
    int       cx      = x1 + (PV_HOME_W - total_w) / 2;
    const int cy      = (PV_BAR_H - SEG_H) / 2;

    for (int i = 0; i < n; i++) {
        const char c = txt[i];
        if (c >= '0' && c <= '9') {
            seg_digit(cx, cy, c - '0', PV_TXT_FG);
        } else if (c == '/') {
            seg_slash(cx, cy, PV_TXT_FG);
        }
        cx += SEG_ADV;
    }

    flush_band(PV_BAR_H);
}

void pv_disp_btn_rect(int which, int *x, int *y, int *w, int *h)
{
    int x0, x1, x2, x3, by;
    bar_layout(&x0, &x1, &x2, &x3, &by);

    if (which < 0) {
        if (x) *x = x0;
        if (y) *y = by + PV_IMG_H;
    } else {
        if (x) *x = x3;
        if (y) *y = by + PV_IMG_H;
    }
    if (w) *w = PV_BTN_W;
    if (h) *h = PV_BTN_H;
}

void pv_disp_home_rect(int *x, int *y, int *w, int *h)
{
    int hx = 0, hy = 0, hw = 0, hh = 0;
    home_layout(&hx, &hy, &hw, &hh);
    if (x) *x = hx;
    if (y) *y = hy;
    if (w) *w = hw;
    if (h) *h = hh;
}

void pv_disp_del_rect(int *x, int *y, int *w, int *h)
{
    int dx = 0, dy = 0, dw = 0, dh = 0;
    del_layout(&dx, &dy, &dw, &dh);
    if (x) *x = dx;
    if (y) *y = dy;
    if (w) *w = dw;
    if (h) *h = dh;
}
