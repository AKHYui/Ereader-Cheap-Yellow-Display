#include "rd_font.h"

#include "nui_ui.h"
#include "pv_config.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "rd_font";

extern const uint8_t _binary_rd_font16_bin_start[];
extern const uint8_t _binary_rd_font16_bin_end[];
extern const uint8_t _binary_rd_font16_ext_bin_start[];
extern const uint8_t _binary_rd_font16_ext_bin_end[];

#define HDR_SIZE      24
#define OFF_MAGIC      0
#define OFF_CELL_W     4
#define OFF_CELL_H     6
#define OFF_BPP        8
#define OFF_ROW_BYTES  9
#define OFF_COUNT     12
#define OFF_BMP       16

typedef struct {
    const uint8_t *cps;
    const uint8_t *bmp;
    uint32_t       count;
    uint32_t       glyph_bytes;
    int            row_bytes;
    int            bpp;
} rd_sec_t;

static rd_sec_t s_sec[2];
static int      s_nsec;
static bool     s_tried;
static bool     s_ok;

static uint32_t cps_at(const rd_sec_t *s, uint32_t i)
{

    uint16_t v;
    memcpy(&v, s->cps + i * 2u, sizeof(v));
    return v;
}

static bool load_sec(rd_sec_t *s, const uint8_t *p, size_t n, const char *what)
{
    if (n < HDR_SIZE || memcmp(p + OFF_MAGIC, "RDF1", 4) != 0) {
        ESP_LOGE(TAG, "%s 头不对（共 %u 字节）—— 是否忘了跑 tools/gen_rd_font.py？",
                 what, (unsigned)n);
        return false;
    }

    uint16_t cw = 0, ch = 0;
    uint32_t cnt = 0, boff = 0;
    memcpy(&cw,   p + OFF_CELL_W, 2);
    memcpy(&ch,   p + OFF_CELL_H, 2);
    memcpy(&cnt,  p + OFF_COUNT, 4);
    memcpy(&boff, p + OFF_BMP,   4);
    const int bpp = p[OFF_BPP];
    const int rb  = p[OFF_ROW_BYTES];

    if (cw != RD_CELL_W || ch != RD_CELL_H) {
        ESP_LOGE(TAG, "%s 格子尺寸不符：blob %ux%u，代码要求 %dx%d",
                 what, cw, ch, RD_CELL_W, RD_CELL_H);
        return false;
    }
    if (bpp != 2 && bpp != 4) {
        ESP_LOGE(TAG, "%s 位深不支持：%d（只支持 2 或 4）", what, bpp);
        return false;
    }
    if (rb * 8 != RD_CELL_W * bpp) {
        ESP_LOGE(TAG, "%s 每行字节数不符：%d（%dbpp 下应为 %d）",
                 what, rb, bpp, RD_CELL_W * bpp / 8);
        return false;
    }
    if (cnt == 0 || boff < HDR_SIZE || boff > n) {
        ESP_LOGE(TAG, "%s 码点数或位图偏移不合理：cnt=%u bmp@%u",
                 what, (unsigned)cnt, (unsigned)boff);
        return false;
    }
    if ((uint64_t)boff + (uint64_t)cnt * ch * rb > (uint64_t)n) {
        ESP_LOGE(TAG, "%s 位图长度超出 blob：需要 %u，实际只有 %u",
                 what, (unsigned)(boff + (uint64_t)cnt * ch * rb), (unsigned)n);
        return false;
    }

    s->cps         = p + HDR_SIZE;
    s->bmp         = p + boff;
    s->count       = cnt;
    s->row_bytes   = rb;
    s->bpp         = bpp;
    s->glyph_bytes = (uint32_t)ch * (uint32_t)rb;

    ESP_LOGI(TAG, "%s就绪：%u 字形 %ux%u %dbpp，%u 字节（%.1f KiB，占分区 %.1f%%）",
             what, (unsigned)cnt, cw, ch, bpp, (unsigned)n, n / 1024.0,
             n * 100.0 / 3997696.0);
    return true;
}

static void rd_font_init(void)
{
    if (s_tried) return;
    s_tried = true;

    if (load_sec(&s_sec[0], _binary_rd_font16_bin_start,
                 (size_t)(_binary_rd_font16_bin_end - _binary_rd_font16_bin_start),
                 "main")) {
        s_nsec = 1;
        s_ok   = true;
    }

    if (load_sec(&s_sec[s_nsec], _binary_rd_font16_ext_bin_start,
                 (size_t)(_binary_rd_font16_ext_bin_end - _binary_rd_font16_ext_bin_start),
                 "ext")) {
        s_nsec++;
    }
}

bool rd_font_ok(void)
{
    rd_font_init();
    return s_ok;
}

bool rd_font_glyph(uint32_t cp, rd_glyph_t *out)
{
    rd_font_init();
    if (!s_ok || !out || cp == 0 || cp > 0xFFFFu) return false;

    for (int k = 0; k < s_nsec; k++) {
        const rd_sec_t *s = &s_sec[k];

        uint32_t lo = 0, hi = s->count;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (cps_at(s, mid) < cp) lo = mid + 1;
            else                     hi = mid;
        }
        if (lo < s->count && cps_at(s, lo) == cp) {
            out->bm        = s->bmp + (uint64_t)lo * s->glyph_bytes;
            out->row_bytes = s->row_bytes;
            out->bpp       = s->bpp;
            return true;
        }
    }
    return false;
}

bool rd_font_has(uint32_t cp)
{
    rd_glyph_t g;
    return rd_font_glyph(cp, &g);
}

int rd_font_width(const uint32_t *cp, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += rd_font_adv(cp[i]);
    return w;
}

void rd_font_row(uint16_t *row, int sy, int x, int y_top,
                 const uint32_t *cp, int n, uint16_t fg, uint16_t bg)
{
    rd_font_init();
    if (!s_ok || !row || !cp) return;

    const int ly = sy - y_top;
    const int in_box = (ly >= 0 && ly < RD_CELL_H);

    for (int i = 0; i < n; i++) {
        const uint32_t c   = cp[i];
        const int      adv = rd_font_adv(c);

        if (x >= PV_SCR_W) break;
        if (x + adv <= 0) { x += adv; continue; }

        if (in_box) {
            rd_glyph_t g;
            if (rd_font_glyph(c, &g)) {

                const int per  = 8 / g.bpp;
                const int mask = (1 << g.bpp) - 1;
                const uint8_t *pr = g.bm + (uint32_t)ly * (uint32_t)g.row_bytes;

                const int cols = (adv == RD_ADV_ASC) ? RD_ADV_ASC : RD_CELL_W;
                for (int k = 0; k < cols; k++) {
                    const int px = x + k;
                    if (px < 0) continue;
                    if (px >= PV_SCR_W) break;
                    const int lv = (pr[k / per] >> (g.bpp * (per - 1 - k % per))) & mask;
                    if (lv == 0) continue;

                    row[px] = (lv == mask) ? fg : nui_blend_lv(fg, bg, lv, mask);
                }
            } else if (adv == RD_ADV_HAN) {

                const int t = 2, btm = RD_CELL_H - 3;
                const int lfx = x + 2, rgt = x + RD_CELL_W - 3;
                if (ly == t || ly == btm) {
                    for (int px = lfx; px <= rgt; px++) {
                        if (px >= 0 && px < PV_SCR_W) row[px] = fg;
                    }
                } else if (ly > t && ly < btm) {
                    if (lfx >= 0 && lfx < PV_SCR_W) row[lfx] = fg;
                    if (rgt >= 0 && rgt < PV_SCR_W) row[rgt] = fg;
                }
            }
        }
        x += adv;
    }
}

int rd_font_ascii_w(const char *s)
{
    return s ? (int)strlen(s) * RD_ADV_ASC : 0;
}

static void ascii_run(uint16_t *row, int sy, int x, int y_top, const char *s,
                      uint16_t fg, uint16_t bg)
{
    if (!s || !s[0]) return;

    uint32_t cp[64];
    int n = 0;
    for (const char *p = s; *p && n < 64; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < 0x20 || c > 0x7E) c = '?';
        cp[n++] = c;
    }
    rd_font_row(row, sy, x, y_top, cp, n, fg, bg);
}

void rd_font_ascii_row(uint16_t *row, int sy, int x, int y_top, const char *s,
                       uint16_t fg, uint16_t bg)
{
    ascii_run(row, sy, x, y_top, s, fg, bg);
}

void rd_font_ascii_mid(uint16_t *row, int sy, int cx, int y_top, const char *s,
                       uint16_t fg, uint16_t bg)
{
    ascii_run(row, sy, cx - rd_font_ascii_w(s) / 2, y_top, s, fg, bg);
}

int rd_cp_from_utf8(const char *s, uint32_t *out, int max)
{
    if (!s || !out || max <= 0) return 0;

    int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < max) {
        const uint8_t c = *p++;
        if (c < 0x80) { out[n++] = c; continue; }

        int      need = 0;
        uint32_t cp   = 0;
        if ((c & 0xE0) == 0xC0)      { need = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
        else                         { out[n++] = 0xFFFD; continue; }

        bool bad = false;
        for (int i = 0; i < need; i++) {
            if ((*p & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (*p++ & 0x3F);
        }

        out[n++] = bad ? 0xFFFD : cp;
    }
    return n;
}

int rd_cp_fit(const uint32_t *cp, int n, int max_w)
{
    int w = 0, k = 0;
    while (k < n) {
        const int a = rd_font_adv(cp[k]);
        if (w + a > max_w) break;
        w += a;
        k++;
    }
    return k;
}
