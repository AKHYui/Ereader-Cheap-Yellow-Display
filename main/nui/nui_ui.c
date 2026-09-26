#include "nui_ui.h"
#include "nui_ascii.h"
#include "nui_glyphs.h"

#include "esp_log.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "nui_ui";

static void report_missing(uint16_t cp)
{
    static uint16_t seen[24];
    static int      n;

    for (int i = 0; i < n; i++) {
        if (seen[i] == cp) return;
    }
    if (n < (int)(sizeof(seen) / sizeof(seen[0]))) seen[n++] = cp;

    ESP_LOGW(TAG, "字库里没有 U+%04X 这个字，屏上是 '?' —— "
                  "把它加进 tools/gen_nui_glyphs.py 的 CHARS 再重跑", cp);
}

void nui_putpx(uint16_t *row, int x, uint16_t c)
{
    if (x < 0 || x >= PV_SCR_W) return;
    row[x] = c;
}

void nui_hspan(uint16_t *row, int x0, int x1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (x1 > PV_SCR_W) x1 = PV_SCR_W;
    for (int x = x0; x < x1; x++) row[x] = c;
}

uint16_t nui_blend_lv(uint16_t fg, uint16_t bg, int lv, int max_lv)
{
    if (lv <= 0) return bg;
    if (lv >= max_lv || max_lv <= 0) return fg;
    const int inv = max_lv - lv;
    const int r = (((fg >> 11) & 0x1F) * lv + ((bg >> 11) & 0x1F) * inv + max_lv / 2) / max_lv;
    const int g = (((fg >>  5) & 0x3F) * lv + ((bg >>  5) & 0x3F) * inv + max_lv / 2) / max_lv;
    const int b = ((fg & 0x1F) * lv + (bg & 0x1F) * inv + max_lv / 2) / max_lv;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t nui_blend(uint16_t fg, uint16_t bg, int lv)
{
    return nui_blend_lv(fg, bg, lv, 3);
}

void nui_rect_row(uint16_t *row, int sy, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    if (sy < y || sy >= y + h) return;
    nui_hspan(row, x, x + w, c);
}

void nui_frame_row(uint16_t *row, int sy, int x, int y, int w, int h,
                   uint16_t fill, uint16_t edge, int t)
{
    if (w <= 0 || h <= 0) return;
    if (sy < y || sy >= y + h) return;
    if (t < 0) t = 0;

    if (t * 2 >= h || t * 2 >= w) {
        nui_hspan(row, x, x + w, edge);
        return;
    }

    const int ly = sy - y;
    if (ly < t || ly >= h - t) {
        nui_hspan(row, x, x + w, edge);
    } else {
        nui_hspan(row, x, x + w, fill);
        nui_hspan(row, x, x + t, edge);
        nui_hspan(row, x + w - t, x + w, edge);
    }
}

uint16_t nui_utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!s || s[0] == 0) return 0;

    uint16_t cp;
    int      n;

    if (s[0] < 0x80) {
        cp = s[0];
        n  = 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = (uint16_t)(((s[0] & 0x1F) << 6) | (s[1] & 0x3F));
        n  = 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = (uint16_t)(((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F));
        n  = 3;
    } else {
        cp = '?';
        n  = 1;
    }

    for (int i = 1; i < n; i++) {
        if (s[i] == 0) { n = 1; cp = '?'; break; }
    }

    *p += n;
    return cp;
}

int nui_text_w(const char *s)
{
    int w = 0;
    const char *p = s;
    while (p && *p) {
        const uint16_t cp = nui_utf8_next(&p);
        if (cp == 0) break;
        w += (cp < 0x80) ? nui_ascii_w() : nui_glyph_w();
    }
    return w;
}

int nui_text_h(void) { return nui_glyph_h(); }

void nui_text_at(uint16_t *row, int sy, int x, int y_top, const char *s,
                 uint16_t fg, uint16_t bg)
{
    if (!row || !s) return;

    const int gw = nui_glyph_w();
    const int gh = nui_glyph_h();
    const int aw = nui_ascii_w();
    const int ah = nui_ascii_h();

    const int y_asc = y_top + (gh - ah) / 2;

    const char *p = s;
    while (*p) {
        const uint16_t cp = nui_utf8_next(&p);
        if (cp == 0) break;

        if (cp < 0x80) {
            const uint8_t *bm = nui_ascii_find((uint8_t)cp);
            const int ly = sy - y_asc;
            if (bm && ly >= 0 && ly < ah) {
                const uint8_t bits = bm[ly];
                for (int i = 0; i < aw; i++) {
                    if (bits & (0x80 >> i)) nui_putpx(row, x + i, fg);
                }
            }
            x += aw;
        } else {
            const uint8_t *bm = nui_glyph_find(cp);
            if (bm) {
                const int ly = sy - y_top;
                if (ly >= 0 && ly < gh) {
                    const uint8_t *line = bm + (size_t)ly * (gw / 4);
                    for (int i = 0; i < gw; i++) {
                        const uint8_t b  = line[i >> 2];
                        const int     lv = (b >> (6 - 2 * (i & 3))) & 0x03;
                        if (lv) nui_putpx(row, x + i, nui_blend(fg, bg, lv));
                    }
                }
            } else {

                report_missing(cp);

                const uint8_t *q = nui_ascii_find('?');
                const int ly = sy - y_asc;
                if (q && ly >= 0 && ly < ah) {
                    const uint8_t bits = q[ly];
                    for (int i = 0; i < aw; i++) {
                        if (bits & (0x80 >> i)) nui_putpx(row, x + i, NUI_DIM);
                    }
                }
            }
            x += gw;
        }
    }
}

void nui_text_center(uint16_t *row, int sy, int cx, int y_top, const char *s,
                     uint16_t fg, uint16_t bg)
{
    nui_text_at(row, sy, cx - nui_text_w(s) / 2, y_top, s, fg, bg);
}

void nui_text_mid(uint16_t *row, int sy, int x, int y, int h, const char *s,
                  uint16_t fg, uint16_t bg)
{

    nui_text_at(row, sy, x, y + (h - nui_text_h()) / 2, s, fg, bg);
}

void nui_text_mid_center(uint16_t *row, int sy, int cx, int y, int h,
                         const char *s, uint16_t fg, uint16_t bg)
{
    nui_text_mid(row, sy, cx - nui_text_w(s) / 2, y, h, s, fg, bg);
}

void nui_tri_row(uint16_t *row, int sy, int cx, int cy, int size, int dir,
                 uint16_t c)
{

    if (size < 2) return;

    const int h = size / 2 + 1;
    int k = sy - (cy - h / 2);
    if (k < 0 || k >= h) return;
    if (dir == 0) k = h - 1 - k;

    const int half = (h > 1) ? (k * (size / 2)) / (h - 1) : (size / 2);
    nui_hspan(row, cx - half, cx + half + 1, c);
}

void nui_sig_row(uint16_t *row, int sy, int x, int y, int level, uint16_t c)
{
    static const int bars[4] = { 4, 7, 10, 13 };
    if (level <= 0) return;
    if (level > 4) level = 4;

    const int bottom = y + 14;
    if (sy < y || sy >= bottom) return;

    for (int i = 0; i < 4; i++) {
        const int bx = x + i * 4;
        if (i < level && sy >= bottom - bars[i] && sy < bottom) {
            nui_hspan(row, bx, bx + 3, c);
        } else if (i >= level && sy >= bottom - bars[i] && sy < bottom) {

            nui_hspan(row, bx, bx + 3, NUI_DIM);
        }
    }
}

void nui_lock_row(uint16_t *row, int sy, int x, int y, uint16_t c)
{

    const int ly = sy - y;
    if (ly < 0 || ly >= 14) return;

    if (ly < 7) {
        if (ly == 0) {
            nui_hspan(row, x + 3, x + 7, c);
        } else if (ly < 6) {
            nui_hspan(row, x + 3, x + 4, c);
            nui_hspan(row, x + 6, x + 7, c);
        }
    } else {
        const int by = ly - 7;
        if (by == 0 || by == 6) {
            nui_hspan(row, x, x + 10, c);
        } else {
            nui_hspan(row, x, x + 1, c);
            nui_hspan(row, x + 9, x + 10, c);
            if (by == 3) nui_hspan(row, x + 4, x + 6, c);
        }
    }
}

void nui_arrow_row(uint16_t *row, int sy, int x, int y, uint16_t c)
{
    const int ly = sy - y;
    if (ly < 0 || ly >= 11) return;
    const int hw = (ly <= 5) ? ly : (10 - ly);
    nui_hspan(row, x, x + hw + 1, c);
    if (ly == 5) nui_hspan(row, x, x + 9, c);
}

int nui_item_y(int i) { return NUI_ITEM_Y0 + i * (NUI_ITEM_H + NUI_ITEM_GAP); }

void nui_title_row(uint16_t *row, int sy, const char *title)
{
    nui_text_mid_center(row, sy, PV_SCR_W / 2, NUI_TITLE_Y, NUI_TITLE_H,
                        title, NUI_FG, NUI_BG);
    if (sy >= NUI_TITLE_LINE && sy < NUI_TITLE_LINE + 2) {
        nui_hspan(row, NUI_ITEM_X, PV_SCR_W - NUI_ITEM_X, NUI_EDGE);
    }
}

uint16_t nui_item_row(uint16_t *row, int sy, int i, bool pressed)
{
    const uint16_t bg = pressed ? NUI_PRESS : NUI_PANEL;
    nui_frame_row(row, sy, NUI_ITEM_X, nui_item_y(i), NUI_ITEM_W, NUI_ITEM_H,
                  bg, NUI_EDGE, 2);
    return bg;
}

void nui_button_row(uint16_t *row, int sy, int x, int y, int w, int h,
                    const char *label, bool pressed)
{
    const uint16_t bg = pressed ? NUI_PRESS : NUI_PANEL;
    nui_frame_row(row, sy, x, y, w, h, bg, NUI_EDGE, 2);
    if (label && label[0]) {
        nui_text_mid_center(row, sy, x + w / 2, y, h, label, NUI_FG, bg);
    }
}

int nui_ascii2x_w(const char *s)
{
    return s ? (int)strlen(s) * nui_ascii_w() * 2 : 0;
}

void nui_ascii2x_row(uint16_t *row, int sy, int x, int y_top, const char *s,
                     uint16_t fg)
{
    if (!row || !s) return;

    const int ah = nui_ascii_h();
    const int ly = (sy - y_top) >> 1;
    if (ly < 0 || ly >= ah) return;

    int cx = x;
    for (const char *p = s; *p; p++) {
        const uint8_t *bm = nui_ascii_find((uint8_t)*p);
        if (bm) {
            const uint8_t bits = bm[ly];
            for (int i = 0; i < 8; i++) {
                if (bits & (0x80 >> i)) {
                    nui_putpx(row, cx + i * 2, fg);
                    nui_putpx(row, cx + i * 2 + 1, fg);
                }
            }
        }
        cx += nui_ascii_w() * 2;
    }
}
