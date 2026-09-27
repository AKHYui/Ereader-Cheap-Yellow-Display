#include "rd_txt.h"

#include "gbk_map.h"
#include "gbk_text.h"
#include "rd_font.h"
#include "rd_walk.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "rd_txt";

#define PAGE_CAP_MIN   1024
#define PAGE_CAP_MAX  12288

#define IN_BUF_SZ       1024

static FILE    *s_fp;
static bool     s_open;
static bool     s_utf8;
static bool     s_bom;

static char     s_title[256];

static char     s_file[256];
static const char *s_enc_name = "?";

static uint32_t *s_pages;
static int       s_npages;
static int       s_cap;
static int       s_page;
static int       s_bytes;

static uint8_t s_inbuf[IN_BUF_SZ];

typedef struct {
    FILE   *f;
    uint8_t *buf;
    int     len, pos;
    int64_t off;
    int     pushed;
    int64_t pushed_off;
    bool    eof;
} rd_in_t;

static int in_byte(rd_in_t *r)
{
    if (r->pos >= r->len) {
        r->len = (int)fread(r->buf, 1, IN_BUF_SZ, r->f);
        r->pos = 0;
        if (r->len <= 0) { r->eof = true; return -1; }
    }
    r->off++;
    return r->buf[r->pos++];
}

static void in_push(rd_in_t *r, int cp, int64_t off)
{
    r->pushed     = cp;
    r->pushed_off = off;
}

static int in_next_cp(rd_in_t *r, int64_t *off_cur)
{
    if (r->pushed >= 0) {
        const int c = r->pushed;
        r->pushed = -1;
        *off_cur  = r->pushed_off;
        return c;
    }

    *off_cur = r->off;
    const int b = in_byte(r);
    if (b < 0) return -1;

    if (s_utf8) {
        if (b < 0x80) return b;
        const int need = ((b & 0xE0) == 0xC0) ? 1
                       : ((b & 0xF0) == 0xE0) ? 2
                       : ((b & 0xF8) == 0xF0) ? 3 : 0;
        if (need == 0) return 0xFFFD;
        uint32_t cp = (uint32_t)(b & (0x3F >> need));
        for (int i = 0; i < need; i++) {
            const int b2 = in_byte(r);
            if (b2 < 0) return -1;
            if ((b2 & 0xC0) != 0x80) return 0xFFFD;
            cp = (cp << 6) | (uint32_t)(b2 & 0x3F);
        }
        return (int)cp;
    }

    if (b < 0x80) return b;
    const int b2 = in_byte(r);
    if (b2 < 0) return -1;
    const uint32_t cp = gbk_lookup((uint8_t)b, (uint8_t)b2);
    return cp ? (int)cp : 0xFFFD;
}

static uint8_t s_detbuf[4096];

static bool detect_utf8(FILE *f)
{
    uint8_t *b = s_detbuf;
    const size_t n = fread(b, 1, sizeof(s_detbuf), f);
    fseek(f, 0, SEEK_SET);

    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        s_bom = true;
        return true;
    }

    size_t i = 0;
    int multi = 0;
    while (i < n) {
        const uint8_t c = b[i];
        if (c < 0x80) { i++; continue; }
        const int need = ((c & 0xE0) == 0xC0) ? 1
                       : ((c & 0xF0) == 0xE0) ? 2
                       : ((c & 0xF8) == 0xF0) ? 3 : 0;
        if (need == 0) return false;
        if (i + (size_t)need >= n) return multi > 0;
        for (int k = 1; k <= need; k++) {
            if ((b[i + (size_t)k] & 0xC0) != 0x80) return false;
        }
        multi++;
        i += (size_t)need + 1;
    }
    return multi > 0;
}

static bool page_push(int64_t off)
{
    if (s_npages >= PAGE_CAP_MAX) return false;
    if (s_npages >= s_cap) {
        int ncap = s_cap ? s_cap * 2 : PAGE_CAP_MIN;
        if (ncap > PAGE_CAP_MAX) ncap = PAGE_CAP_MAX;
        uint32_t *p = heap_caps_realloc(s_pages, (size_t)ncap * sizeof(uint32_t),
                                       MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGE(TAG, "页表扩容到 %d 项失败（堆不足）", ncap);
            return false;
        }
        s_pages = p;
        s_cap   = ncap;
    }
    s_pages[s_npages++] = (uint32_t)off;
    return true;
}

bool rd_txt_open(const char *path)
{
    rd_txt_close();

    s_fp = fopen(path, "rb");
    if (!s_fp) {
        ESP_LOGE(TAG, "打不开 %s", path);
        return false;
    }

    fseek(s_fp, 0, SEEK_END);
    s_bytes = (int)ftell(s_fp);
    fseek(s_fp, 0, SEEK_SET);

    s_bom  = false;
    s_utf8 = detect_utf8(s_fp);
    s_enc_name = s_utf8 ? (s_bom ? "UTF-8(BOM)" : "UTF-8") : "GBK";

    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;

        rd_name_utf8(base, s_file, sizeof(s_file));

        char tmp[96];
        strncpy(tmp, base, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        char *dot = strrchr(tmp, '.');
        if (dot) *dot = 0;
        rd_name_utf8(tmp, s_title, sizeof(s_title));
    }

    rd_in_t r;
    memset(&r, 0, sizeof(r));
    r.f      = s_fp;
    r.buf    = s_inbuf;
    r.pushed = -1;
    if (s_bom) { in_byte(&r); in_byte(&r); in_byte(&r); }

    rd_walk_t w;
    memset(&w, 0, sizeof(w));

    if (!page_push(r.off)) {
        fclose(s_fp);
        s_fp = NULL;
        return false;
    }

    for (;;) {
        int64_t off_cur = r.off;
        const int v = in_next_cp(&r, &off_cur);
        if (v < 0) break;

        if (w.pending) {
            if (!page_push(off_cur)) {
                ESP_LOGW(TAG, "页表到上限 %d 项，文件后半段不再分页 —— 只能读到前面这些",
                         PAGE_CAP_MAX);
                break;
            }
            w.pending = false;
            w.lines   = 0;
        }
        if (rd_walk_feed(NULL, &w, (uint32_t)v)) {
            in_push(&r, v, off_cur);
        }
    }
    s_open = true;
    s_page = 0;

    if (s_npages <= 0) {
        ESP_LOGE(TAG, "分不出页（文件 %d 字节）", s_bytes);
        rd_txt_close();
        return false;
    }

    ESP_LOGI(TAG, "《%s》%s  %d 字节  分页 %d 页（页表 %d 项，%d 字节堆）",
             s_title, s_enc_name, s_bytes, s_npages, s_cap, s_cap * 4);
    return true;
}

void rd_txt_close(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }

    if (s_pages) {
        heap_caps_free(s_pages);
        s_pages = NULL;
    }
    s_open  = false;
    s_npages = 0;
    s_cap    = 0;
    s_page   = 0;
}

bool rd_txt_is_open(void)          { return s_open; }
const char *rd_txt_title(void)     { return s_title; }
const char *rd_txt_file(void)      { return s_file; }
const char *rd_txt_encoding(void)  { return s_enc_name; }
int  rd_txt_bytes(void)            { return s_bytes; }
int  rd_txt_pages(void)            { return s_npages; }
int  rd_txt_page(void)             { return s_page; }

bool rd_txt_goto(int idx)
{
    if (!s_open || idx < 0 || idx >= s_npages) return false;
    s_page = idx;
    return true;
}

bool rd_txt_next(void) { return rd_txt_goto(s_page + 1); }
bool rd_txt_prev(void) { return rd_txt_goto(s_page - 1); }

int rd_txt_percent(void)
{
    if (!s_open || s_npages <= 1) return 0;
    return (int)((int64_t)s_page * 100 / (s_npages - 1));
}

bool rd_txt_load_page(rd_page_t *out)
{
    if (!s_open || !out) return false;
    if (s_page < 0 || s_page >= s_npages) return false;

    memset(out, 0, sizeof(*out));

    if (fseek(s_fp, (long)s_pages[s_page], SEEK_SET) != 0) {
        ESP_LOGE(TAG, "fseek 到第 %d 页（偏移 %u）失败", s_page, (unsigned)s_pages[s_page]);
        return false;
    }

    rd_in_t r;
    memset(&r, 0, sizeof(r));
    r.f      = s_fp;
    r.buf    = s_inbuf;
    r.off    = (int64_t)s_pages[s_page];
    r.pushed = -1;

    rd_walk_t w;
    memset(&w, 0, sizeof(w));

    for (;;) {
        int64_t off_cur = r.off;
        const int v = in_next_cp(&r, &off_cur);
        if (v < 0) break;
        if (w.lines >= RD_LINES_PER_PAGE) break;

        if (rd_walk_feed(out, &w, (uint32_t)v)) {
            in_push(&r, v, off_cur);

        }
    }

    rd_walk_finish(out, &w);
    return true;
}

static bool utf8_valid(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        const uint8_t c = *p++;
        if (c < 0x80) continue;
        const int need = ((c & 0xE0) == 0xC0) ? 1
                       : ((c & 0xF0) == 0xE0) ? 2
                       : ((c & 0xF8) == 0xF0) ? 3 : 0;
        if (need == 0) return false;
        for (int i = 0; i < need; i++) {
            if ((*p & 0xC0) != 0x80) return false;
            p++;
        }
    }
    return true;
}

void rd_name_utf8(const char *src, char *dst, int dst_len)
{
    if (!src || !dst || dst_len <= 0) return;

    if (utf8_valid(src)) {
        strncpy(dst, src, (size_t)dst_len - 1);
        dst[dst_len - 1] = 0;
    } else {
        gbk_to_utf8(src, dst, dst_len);
    }
}
