#include "rd_epub.h"

#include "pv_disp.h"
#include "rd_font.h"
#include "rd_txt.h"
#include "rd_walk.h"

#include "miniz.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "rd_epub";

#define EP_MAX_ENT    128

#define EP_POOL_SZ    4096

#define EP_MAX_CHAP   192
#define EP_MAX_ID     192
#define EP_ID_LEN      24
#define EP_NAME_MAX   255
#define EP_TAG_MAX    384

#define EP_IN_SZ      2048
#define EP_DICT_SZ    TINFL_LZ_DICT_SIZE

_Static_assert(EP_DICT_SZ == 32768, "miniz 的 LZ 字典必须是 32KB");

typedef struct {
    uint32_t name_off;
    uint32_t lho;
    uint32_t csize;
    uint32_t usize;
    uint16_t method;
} ep_ent_t;

typedef struct {
    uint32_t lho;
    uint32_t csize;
    uint16_t method;
    uint16_t pages;
    uint32_t first_page;
    int      ent;
} ep_chap_t;

static FILE    *s_fp;
static bool     s_open;
static int32_t  s_fsize;
static char     s_title[256];
static char     s_file[256];

static ep_chap_t s_chap[EP_MAX_CHAP];
static int       s_nchap;
static int       s_npages;
static int       s_page;

static tinfl_decompressor *s_dec;
static uint8_t            *s_dict;

static uint8_t s_in[EP_IN_SZ];
static uint8_t s_copy[512];

static ep_ent_t *s_ent;
static char     *s_pool;
static int       s_nent;
static size_t    s_pool_used;

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool ep_find_eocd(uint32_t *cd_off, uint32_t *cd_size, uint16_t *cnt)
{
    static const int WIN = 1024;
    int32_t back  = 22;
    const int32_t limit = 22 + 65535 + 4;

    while (back <= limit && back <= s_fsize) {
        const int32_t start = s_fsize - back;
        const int32_t want  = (back < WIN) ? back : WIN;
        if (start < 0) return false;

        if (fseek(s_fp, (long)start, SEEK_SET) != 0) return false;
        const int got = (int)fread(s_in, 1, (size_t)want, s_fp);
        if (got < 22) { back += WIN - 3; continue; }

        for (int i = got - 22; i >= 0; i--) {
            if (rd32le(s_in + i) != 0x06054b50u) continue;
            const uint32_t o = rd32le(s_in + i + 16);
            const uint32_t z = rd32le(s_in + i + 12);
            if (o + z > (uint32_t)s_fsize) continue;
            *cd_off = o; *cd_size = z; *cnt = rd16le(s_in + i + 10);
            return true;
        }
        back += WIN - 3;
    }
    return false;
}

static bool ep_want_name(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    dot++;
    return !strcasecmp(dot, "xhtml") || !strcasecmp(dot, "html") ||
           !strcasecmp(dot, "htm")   || !strcasecmp(dot, "opf")  ||
           !strcasecmp(dot, "xml")   || !strcasecmp(dot, "ncx");
}

static const char *ent_name(int i) { return &s_pool[s_ent[i].name_off]; }

static bool ep_parse_cd(uint32_t cd_off, uint16_t cnt)
{
    static uint8_t hdr[46];
    static uint8_t nm[EP_NAME_MAX + 1];

    if (fseek(s_fp, (long)cd_off, SEEK_SET) != 0) return false;

    s_nent = 0;
    s_pool_used = 0;
    int skipped = 0;

    for (uint16_t i = 0; i < cnt; i++) {
        if (fread(hdr, 1, 46, s_fp) != 46) break;
        if (rd32le(hdr) != 0x02014b50u) break;

        const uint16_t method = rd16le(hdr + 10);
        const uint32_t csize  = rd32le(hdr + 20);
        const uint32_t usize  = rd32le(hdr + 24);
        const uint16_t nlen   = rd16le(hdr + 28);
        const uint16_t elen   = rd16le(hdr + 30);
        const uint16_t clen   = rd16le(hdr + 32);
        const uint32_t lho    = rd32le(hdr + 42);

        if (nlen == 0 || nlen > EP_NAME_MAX) {
            if (fseek(s_fp, (long)(nlen + elen + clen), SEEK_CUR) != 0) break;
            continue;
        }
        if (fread(nm, 1, nlen, s_fp) != nlen) break;
        nm[nlen] = 0;
        if (fseek(s_fp, (long)(elen + clen), SEEK_CUR) != 0) break;

        if (!ep_want_name((const char *)nm)) continue;
        if (s_nent >= EP_MAX_ENT) {
            ESP_LOGW(TAG, "条目表满（%d），「%s」之后不再收录", EP_MAX_ENT, nm);
            break;
        }
        if (s_pool_used + nlen + 1 > EP_POOL_SZ) {

            ESP_LOGW(TAG, "名字池满，「%s」起不再收录", nm);
            break;
        }

        ep_ent_t *e = &s_ent[s_nent];
        e->name_off = (uint32_t)s_pool_used;
        e->lho      = lho;
        e->csize    = csize;
        e->usize    = usize;
        e->method   = method;
        memcpy(&s_pool[s_pool_used], nm, (size_t)nlen + 1);
        s_pool_used += (size_t)nlen + 1;
        s_nent++;
    }

    ESP_LOGI(TAG, "中央目录：%u 条里收录 %d 条（名字池 %u/%d 字节）",
             (unsigned)cnt, s_nent, (unsigned)s_pool_used, EP_POOL_SZ);
    (void)skipped;
    return s_nent > 0;
}

static int ep_find(const char *name)
{
    for (int i = 0; i < s_nent; i++) {
        if (!strcmp(ent_name(i), name)) return i;
    }
    return -1;
}

static int ep_find_suffix(const char *href)
{
    const size_t hl = strlen(href);
    for (int i = 0; i < s_nent; i++) {
        const char *n   = ent_name(i);
        const size_t nl = strlen(n);
        if (nl < hl) continue;
        if (strcmp(n + (nl - hl), href)) continue;
        if (nl == hl || n[nl - hl - 1] == '/') return i;
    }
    return -1;
}

typedef void (*ep_out_fn)(void *ctx, const uint8_t *p, int n);

static bool ep_seek_data(uint32_t lho)
{
    static uint8_t lh[30];
    if (fseek(s_fp, (long)lho, SEEK_SET) != 0) return false;
    if (fread(lh, 1, 30, s_fp) != 30) return false;
    if (rd32le(lh) != 0x04034b50u) return false;
    const uint16_t nlen = rd16le(lh + 26);
    const uint16_t elen = rd16le(lh + 28);
    return fseek(s_fp, (long)(lho + 30u + nlen + elen), SEEK_SET) == 0;
}

static bool s_dict_borrowed;

static uint8_t *ep_dict_acquire(void)
{
    uint8_t *p = (uint8_t *)pv_disp_scratch(EP_DICT_SZ);
    s_dict_borrowed = (p != NULL);
    if (!p) p = heap_caps_malloc(EP_DICT_SZ, MALLOC_CAP_8BIT);

    s_dict = p;
    return p;
}

static void ep_dict_release(void)
{
    if (s_dict && !s_dict_borrowed) heap_caps_free(s_dict);
    s_dict          = NULL;
    s_dict_borrowed = false;
}

static bool ep_inflate_at(uint32_t lho, uint32_t csize, uint16_t method,
                          const char *what, ep_out_fn out, void *ctx)
{
    if (!ep_seek_data(lho)) {
        ESP_LOGW(TAG, "本地头异常：%s", what);
        return false;
    }

    if (method == 0) {
        uint32_t left = csize;
        while (left) {
            const int want = left > (uint32_t)sizeof(s_copy)
                           ? (int)sizeof(s_copy) : (int)left;
            const int got = (int)fread(s_copy, 1, (size_t)want, s_fp);
            if (got <= 0) return false;
            if (out) out(ctx, s_copy, got);
            left -= (uint32_t)got;
        }
        return true;
    }
    if (method != 8) {
        ESP_LOGW(TAG, "不支持的压缩方式 %u：%s", (unsigned)method, what);
        return false;
    }
    if (!s_dec) return false;

    uint8_t *dict = ep_dict_acquire();
    if (!dict) {
        ESP_LOGW(TAG, "拿不到 %d 字节的 LZ 字典：%s", EP_DICT_SZ, what);
        return false;
    }

    tinfl_init(s_dec);

    uint32_t left     = csize;
    int      in_len = 0, in_pos = 0;
    size_t   dict_ofs = 0;
    int      guard = 0;
    bool     ok    = false;

    for (;;) {
        if (in_pos >= in_len) {
            if (left > 0) {
                const int want = (left > EP_IN_SZ) ? EP_IN_SZ : (int)left;
                const int got  = (int)fread(s_in, 1, (size_t)want, s_fp);
                if (got <= 0) {
                    ESP_LOGW(TAG, "压缩数据读不满：%s", what);
                    break;
                }
                in_len = got;
                in_pos = 0;
                left  -= (uint32_t)got;
            } else {
                in_len = 0;
                in_pos = 0;
            }
        }

        size_t         avail    = (size_t)(in_len - in_pos);
        size_t         produced = EP_DICT_SZ - dict_ofs;

        const uint32_t flags = (left > 0) ? TINFL_FLAG_HAS_MORE_INPUT : 0;

        const tinfl_status st = tinfl_decompress(
            s_dec, s_in + in_pos, &avail,
            dict, dict + dict_ofs, &produced, flags);

        in_pos  += (int)avail;
        if (produced && out) out(ctx, dict + dict_ofs, (int)produced);
        dict_ofs = (dict_ofs + produced) & (EP_DICT_SZ - 1);

        if (st == TINFL_STATUS_DONE) { ok = true; break; }
        if (st < 0) {
            ESP_LOGW(TAG, "解压失败 status=%d：%s", (int)st, what);
            break;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && left == 0 && in_pos >= in_len) {
            ESP_LOGW(TAG, "压缩数据提前结束：%s", what);
            break;
        }

        if (avail == 0 && produced == 0) {
            if (++guard > 4) {
                ESP_LOGW(TAG, "解压空转：%s", what);
                break;
            }
        } else {
            guard = 0;
        }
    }

    ep_dict_release();
    return ok;
}

static bool ep_inflate(int ei, ep_out_fn out, void *ctx)
{
    return ep_inflate_at(s_ent[ei].lho, s_ent[ei].csize, s_ent[ei].method,
                         ent_name(ei), out, ctx);
}

typedef void (*ep_tag_fn)(void *ctx, const char *tag);

typedef struct {
    int       state;
    char      buf[EP_TAG_MAX];
    int       len;
    int       overflow;
    ep_tag_fn fn;
    void     *ctx;
} ep_xml_t;

static void ep_xml_byte(ep_xml_t *x, uint8_t b)
{
    if (x->state == 0) {
        if (b == '<') { x->state = 1; x->len = 0; x->overflow = 0; }
        return;
    }
    if (x->state == 2) {
        if (b == '>') x->state = 0;
        return;
    }
    if (b == '>') {
        x->buf[x->len] = 0;
        if (x->fn && x->len) x->fn(x->ctx, x->buf);
        x->state = 0;
    } else if (b == '!' || b == '?') {
        x->state = 2;
    } else if (x->len < EP_TAG_MAX - 1) {
        x->buf[x->len++] = (char)b;
    } else if (!x->overflow) {
        x->overflow = 1;
        ESP_LOGW(TAG, "标签超过 %d 字节，已截断（结构异常？）", EP_TAG_MAX);
    }
}

static void xml_out(void *ctx, const uint8_t *p, int n)
{
    ep_xml_t *x = (ep_xml_t *)ctx;
    for (int i = 0; i < n; i++) ep_xml_byte(x, p[i]);
}

static bool ep_attr(const char *tag, const char *key, char *out, int outsz)
{
    const size_t kl = strlen(key);
    const char  *p  = tag;

    while ((p = strstr(p, key)) != NULL) {
        if (p != tag) {
            const char c = p[-1];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':') {
                p += kl;
                continue;
            }
        }
        const char *q = p + kl;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '=') { p += kl; continue; }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"' && *q != '\'') { p += kl; continue; }
        const char  quo = *q++;
        const char *end = strchr(q, quo);
        if (!end) return false;
        int n = (int)(end - q);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, q, (size_t)n);
        out[n] = 0;
        return n > 0;
    }
    return false;
}

static bool tag_is(const char *tag, const char *name)
{
    if (*tag == '/') tag++;
    while (*tag == ' ' || *tag == '\t') tag++;
    const size_t nl = strlen(name);
    if (strncasecmp(tag, name, nl)) return false;
    const char c = tag[nl];
    return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/';
}

typedef void (*ep_text_fn)(void *ctx, const char *bytes, int n);

typedef struct {
    int        state;
    int        skip;
    char       tag[16];
    int        tlen;
    int        ws;
    int        emitted;
    int        line_start;

    int        ent_on;
    int        ent_mode;
    uint32_t   ent_val;
    char       ent_name[10];
    int        ent_len;
    ep_text_fn fn;
    void      *ctx;
} ep_html_t;

static bool html_is_block(const char *t)
{
    static const char *const B[] = {
        "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6",
        "li", "tr", "td", "blockquote", "section", "article", "hr", "nav",
    };
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); i++) {
        if (!strcmp(t, B[i])) return true;
    }
    return false;
}

static bool html_skips(const char *t)
{
    static const char *const S[] = { "head", "style", "script", "rt", "rp", "svg", "title" };
    for (size_t i = 0; i < sizeof(S) / sizeof(S[0]); i++) {
        if (!strcmp(t, S[i])) return true;
    }
    return false;
}

static void hput(ep_html_t *h, const char *p, int n)
{
    if (n > 0 && h->fn) h->fn(h->ctx, p, n);
}

static void hput_cp(ep_html_t *h, uint32_t cp)
{
    char b[4];
    int  n = 0;
    if (cp < 0x80) {
        b[n++] = (char)cp;
    } else if (cp < 0x800) {
        b[n++] = (char)(0xC0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        b[n++] = (char)(0xE0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        b[n++] = (char)(0xF0 | (cp >> 18));
        b[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    }
    hput(h, b, n);
    h->emitted    = 1;
    h->line_start = 0;
}

static void hput_char(ep_html_t *h, uint8_t b)
{
    if (h->ws) {
        h->ws = 0;
        if (h->emitted && !h->line_start) hput(h, " ", 1);
    }
    hput(h, (const char *)&b, 1);
    h->emitted    = 1;
    h->line_start = 0;
}

static void hnewline(ep_html_t *h)
{
    h->ws = 0;
    if (h->emitted) hput(h, "\n", 1);
    h->line_start = 1;
}

static void html_tag_end(ep_html_t *h)
{
    char       name[16];
    int        n = 0;
    const char *p = h->tag;
    const bool  closing = (*p == '/');
    if (closing) p++;

    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '/' &&
           n < (int)sizeof(name) - 1) {
        char c = *p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        name[n++] = c;
    }
    name[n] = 0;
    if (!n) return;

    if (html_skips(name)) {
        if (closing) { if (h->skip > 0) h->skip--; }
        else         { h->skip++; }
        h->ws = 0;
        return;
    }
    if (h->skip) return;
    if (html_is_block(name)) hnewline(h);
}

static void html_ent_finish(ep_html_t *h)
{
    uint32_t cp = 0;
    if (h->ent_mode == 0) {
        static const struct { const char *n; uint32_t c; } E[] = {
            { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
            { "apos", '\'' }, { "nbsp", ' ' }, { "mdash", 0x2014u },
            { "ndash", 0x2013u }, { "hellip", 0x2026u }, { "lsquo", 0x2018u },
            { "rsquo", 0x2019u }, { "ldquo", 0x201Cu }, { "rdquo", 0x201Du },
        };
        h->ent_name[h->ent_len] = 0;
        for (size_t i = 0; i < sizeof(E) / sizeof(E[0]); i++) {
            if (!strcmp(h->ent_name, E[i].n)) { cp = E[i].c; break; }
        }
        if (!cp) { hput(h, "&", 1); hput(h, h->ent_name, h->ent_len); }
    } else {
        cp = h->ent_val;
        if (cp == 0) { hput(h, "&", 1); }
    }
    if (cp) hput_cp(h, cp);
    h->ent_on  = 0;
    h->ent_len = 0;
    h->ent_val = 0;
}

static void html_byte(ep_html_t *h, uint8_t b)
{
    if (h->state == 1) {
        if (b == '>') {
            h->tag[h->tlen] = 0;
            html_tag_end(h);
            h->state = 0;
            h->tlen  = 0;
        } else if (b == '!' || b == '?') {
            h->state = 2;
        } else if (h->tlen < (int)sizeof(h->tag) - 1) {
            h->tag[h->tlen++] = (char)b;
        }
        return;
    }
    if (h->state == 2) {
        if (b == '>') h->state = 0;
        return;
    }

    if (b == '<') { h->state = 1; h->tlen = 0; h->ent_on = 0; return; }

    if (h->skip) return;

    if (h->ent_on) {
        if (b == ';') { html_ent_finish(h); return; }
        if (b == '#') { h->ent_mode = 1; return; }
        if ((b == 'x' || b == 'X') && h->ent_mode == 1) { h->ent_mode = 2; return; }
        if (b >= '0' && b <= '9') {
            h->ent_val = (h->ent_mode == 2) ? (h->ent_val << 4) | (uint32_t)(b - '0')
                                            : h->ent_val * 10u + (uint32_t)(b - '0');
            return;
        }
        if (h->ent_mode == 2 && ((b | 0x20) >= 'a' && (b | 0x20) <= 'f')) {
            h->ent_val = (h->ent_val << 4) | (uint32_t)((b | 0x20) - 'a' + 10);
            return;
        }
        if (h->ent_mode == 0 && ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')) &&
            h->ent_len < (int)sizeof(h->ent_name) - 1) {
            h->ent_name[h->ent_len++] = (char)b;
            return;
        }

        h->ent_on = 0;
        hput(h, "&", 1);
        if (h->ent_len) hput(h, h->ent_name, h->ent_len);
        h->ent_len = 0;
        h->ent_val = 0;
    }

    if (b == '&') {
        h->ent_on = 1; h->ent_mode = 0; h->ent_len = 0; h->ent_val = 0;
        return;
    }
    if (b == ' ' || b == '\t' || b == '\r' || b == '\n') { h->ws = 1; return; }
    hput_char(h, b);
}

typedef struct {
    rd_page_t *out;
    int        want;
    int        page;
    bool       stop;
    bool       by_full;
    rd_walk_t  w;
    int32_t    push;
    bool       has_push;
} ep_run_t;

static void run_cp(ep_run_t *R, uint32_t cp)
{
    if (R->stop) return;

    for (int spin = 0; spin < 4; spin++) {
        if (R->has_push) { cp = (uint32_t)R->push; R->has_push = false; }

        if (R->w.pending) {
            R->page++;
            R->w.pending = false;
            R->w.lines   = 0;
            if (R->out && R->page > R->want) {
                R->stop    = true;
                R->by_full = true;
                return;
            }
        }

        rd_page_t *dst = (R->out && R->page == R->want) ? R->out : NULL;
        if (rd_walk_feed(dst, &R->w, cp)) {
            R->push     = (int32_t)cp;
            R->has_push = true;
            continue;
        }
        return;
    }
    ESP_LOGW(TAG, "同一个码点反复回推，丢弃（U+%04X）", (unsigned)cp);
}

typedef struct {
    ep_html_t h;
    ep_run_t *R;
    uint32_t  cp;
    int       need, have;
} ep_sink_t;

static void sink_text(void *ctx, const char *bytes, int n)
{
    ep_sink_t *S = (ep_sink_t *)ctx;
    for (int i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)bytes[i];
        if (S->have == 0) {
            if (c < 0x80) { run_cp(S->R, c); continue; }
            if ((c & 0xE0) == 0xC0) { S->cp = c & 0x1Fu; S->need = 1; S->have = 1; continue; }
            if ((c & 0xF0) == 0xE0) { S->cp = c & 0x0Fu; S->need = 2; S->have = 1; continue; }
            if ((c & 0xF8) == 0xF0) { S->cp = c & 0x07u; S->need = 3; S->have = 1; continue; }
            run_cp(S->R, 0xFFFD);
            continue;
        }
        if ((c & 0xC0) != 0x80) { S->have = 0; run_cp(S->R, 0xFFFD); continue; }
        S->cp = (S->cp << 6) | (c & 0x3Fu);
        if (++S->have > S->need) {
            S->have = 0;
            run_cp(S->R, S->cp);
        }
    }
}

static void sink_bytes(void *ctx, const uint8_t *p, int n)
{
    ep_sink_t *S = (ep_sink_t *)ctx;
    for (int i = 0; i < n; i++) html_byte(&S->h, p[i]);
}

static int ep_chap_walk(int ci, rd_page_t *out, int want)
{
    static ep_sink_t S;
    static ep_run_t  R;

    memset(&S, 0, sizeof(S));
    S.h.fn  = sink_text;
    S.h.ctx = &S;

    memset(&R, 0, sizeof(R));
    R.out   = out;
    R.want  = want;
    S.R     = &R;

    if (out) memset(out, 0, sizeof(*out));

    (void)ep_inflate_at(s_chap[ci].lho, s_chap[ci].csize, s_chap[ci].method,
                        "chapter", sink_bytes, &S);

    int pages;
    if (R.stop) {
        pages = R.page;
        if (out && R.by_full) out->lines = RD_LINES_PER_PAGE;
    } else {
        if (out) rd_walk_finish(out, &R.w);
        pages = R.page;
        if (R.w.lines > 0 || (R.w.line_n > 0 && R.w.line_text)) pages++;
    }
    return pages;
}

typedef struct {
    int          mode;
    int          n;
    char       (*ids)[EP_ID_LEN];
    const char  *dir;
} ep_opf_ctx_t;

static void opf_tag(void *ctx, const char *tag)
{
    ep_opf_ctx_t *O = (ep_opf_ctx_t *)ctx;
    char v[EP_NAME_MAX + 1];

    if (O->mode == 0) {
        if (!tag_is(tag, "itemref")) return;
        if (O->n >= EP_MAX_ID) return;
        if (!ep_attr(tag, "idref", v, sizeof(v))) return;
        strncpy(O->ids[O->n], v, EP_ID_LEN - 1);
        O->ids[O->n][EP_ID_LEN - 1] = 0;
        O->n++;
        return;
    }

    if (!tag_is(tag, "item")) return;
    char id[EP_ID_LEN], href[EP_NAME_MAX + 1];
    if (!ep_attr(tag, "id", id, sizeof(id))) return;
    if (!ep_attr(tag, "href", href, sizeof(href))) return;

    int slot = -1;
    for (int i = 0; i < O->n; i++) {
        if (!strcmp(O->ids[i], id)) { slot = i; break; }
    }
    if (slot < 0) return;
    if (s_chap[slot].lho) return;

    char *hash = strchr(href, '#');
    if (hash) *hash = 0;

    char clean[EP_NAME_MAX + 1];
    int  m = 0;
    for (int i = 0; href[i] && m < EP_NAME_MAX; i++) {
        if (href[i] == '%' && href[i + 1] && href[i + 2]) {
            const int hi = (href[i + 1] <= '9') ? href[i + 1] - '0' : (href[i + 1] | 0x20) - 'a' + 10;
            const int lo = (href[i + 2] <= '9') ? href[i + 2] - '0' : (href[i + 2] | 0x20) - 'a' + 10;
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                clean[m++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        clean[m++] = href[i];
    }
    clean[m] = 0;
    while (clean[0] == '.' && clean[1] == '/') memmove(clean, clean + 2, strlen(clean + 1));

    int ei = -1;
    if (O->dir[0]) {
        char full[EP_NAME_MAX + 200];
        snprintf(full, sizeof(full), "%s%s", O->dir, clean);
        ei = ep_find(full);
    }
    if (ei < 0) ei = ep_find(clean);
    if (ei < 0) ei = ep_find_suffix(clean);
    if (ei < 0) {
        ESP_LOGW(TAG, "spine 里的 %s 在 ZIP 里找不到，跳过", clean);
        return;
    }

    s_chap[slot].lho    = s_ent[ei].lho;
    s_chap[slot].csize  = s_ent[ei].csize;
    s_chap[slot].method = s_ent[ei].method;
    s_chap[slot].ent    = ei;
    if (slot + 1 > s_nchap) s_nchap = slot + 1;
}

static void rootfile_tag(void *ctx, const char *tag)
{
    char *out = (char *)ctx;
    if (out[0]) return;
    if (!tag_is(tag, "rootfile")) return;
    ep_attr(tag, "full-path", out, EP_NAME_MAX + 1);
}

static void free_tables(void)
{
    if (s_ent)  { heap_caps_free(s_ent);  s_ent  = NULL; }
    if (s_pool) { heap_caps_free(s_pool); s_pool = NULL; }
    s_nent = 0;
    s_pool_used = 0;
}

void rd_epub_close(void)
{
    if (s_fp) { fclose(s_fp); s_fp = NULL; }

    if (s_dec)  { heap_caps_free(s_dec);  s_dec  = NULL; }
    ep_dict_release();
    free_tables();
    s_open   = false;
    s_nchap  = 0;
    s_npages = 0;
    s_page   = 0;
}

bool rd_epub_mem_ready(void)
{

    const size_t need = sizeof(tinfl_decompressor)
                      + sizeof(ep_ent_t) * EP_MAX_ENT
                      + (size_t)EP_POOL_SZ
                      + (size_t)EP_ID_LEN * EP_MAX_ID;

    void *dict = ep_dict_acquire();
    void *dec  = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    void *ent  = heap_caps_malloc(sizeof(ep_ent_t) * EP_MAX_ENT, MALLOC_CAP_8BIT);
    void *pool = heap_caps_malloc(EP_POOL_SZ, MALLOC_CAP_8BIT);
    void *ids  = heap_caps_malloc((size_t)EP_ID_LEN * EP_MAX_ID, MALLOC_CAP_8BIT);

    const bool ok = dict && dec && ent && pool && ids;
    ESP_LOGW(TAG, "EPUB 内存前提：%s —— 堆要 %u，要完后剩 空闲 %u / 最大块 %u"
                  " | 字典(借行带)%s 解压器%s 条目表%s 名字池%s id表%s",
             ok ? "可以（遥控读 EPUB 走得通）" : "**不够（遥控读 EPUB 会打不开）**",
             (unsigned)need,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             dict ? "ok" : "×", dec ? "ok" : "×", ent ? "ok" : "×",
             pool ? "ok" : "×", ids ? "ok" : "×");

    if (ids)  heap_caps_free(ids);
    if (pool) heap_caps_free(pool);
    if (ent)  heap_caps_free(ent);
    if (dec)  heap_caps_free(dec);
    if (dict) ep_dict_release();
    return ok;
}

bool rd_epub_open(const char *path, rd_prog_fn prog)
{
    rd_epub_close();

    s_fp = fopen(path, "rb");
    if (!s_fp) { ESP_LOGE(TAG, "打不开 %s", path); return false; }

    fseek(s_fp, 0, SEEK_END);
    s_fsize = (int32_t)ftell(s_fp);
    fseek(s_fp, 0, SEEK_SET);

    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        rd_name_utf8(base, s_file, sizeof(s_file));
        char tmp[256];
        strncpy(tmp, base, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        char *dot = strrchr(tmp, '.');
        if (dot) *dot = 0;
        rd_name_utf8(tmp, s_title, sizeof(s_title));
    }

    s_dec  = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT);
    s_ent  = heap_caps_malloc(sizeof(ep_ent_t) * EP_MAX_ENT, MALLOC_CAP_8BIT);
    s_pool = heap_caps_malloc(EP_POOL_SZ, MALLOC_CAP_8BIT);
    if (!s_dec || !s_ent || !s_pool) {
        ESP_LOGE(TAG, "内存不足（空闲 %u / 最大可分配块 %u）—— 解压器 %u + 条目表 %u + 名字池 %d",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)sizeof(tinfl_decompressor),
                 (unsigned)(sizeof(ep_ent_t) * EP_MAX_ENT), EP_POOL_SZ);
        rd_epub_close();
        return false;
    }

    s_dict = ep_dict_acquire();
    if (!s_dict) {
        ESP_LOGE(TAG, "LZ 字典（%d 字节）拿不到：行带外借不可用，堆也给不出连续块",
                 EP_DICT_SZ);
        rd_epub_close();
        return false;
    }
    ep_dict_release();

    uint32_t cd_off = 0, cd_size = 0;
    uint16_t cnt = 0;
    if (!ep_find_eocd(&cd_off, &cd_size, &cnt)) {
        ESP_LOGE(TAG, "找不到 ZIP 的 EOCD —— 不是 ZIP/EPUB，或文件被截断");
        rd_epub_close();
        return false;
    }
    if (!ep_parse_cd(cd_off, cnt)) {
        ESP_LOGE(TAG, "中央目录里没有可读的 xml/xhtml 条目");
        rd_epub_close();
        return false;
    }

    char opf[EP_NAME_MAX + 1];
    opf[0] = 0;
    {
        const int ci = ep_find("META-INF/container.xml");
        if (ci < 0) {
            ESP_LOGW(TAG, "没有 META-INF/container.xml，改为全表找 .opf");
        } else {
            ep_xml_t x;
            memset(&x, 0, sizeof(x));
            x.fn  = rootfile_tag;
            x.ctx = opf;
            ep_inflate(ci, xml_out, &x);
        }
    }
    if (!opf[0]) {
        for (int i = 0; i < s_nent; i++) {
            const char *n = ent_name(i);
            const size_t nl = strlen(n);
            if (nl > 4 && !strcasecmp(n + nl - 4, ".opf")) {
                strncpy(opf, n, sizeof(opf) - 1);
                opf[sizeof(opf) - 1] = 0;
                ESP_LOGW(TAG, "container.xml 没给出 OPF，改用 %s", opf);
                break;
            }
        }
    }
    if (!opf[0]) {
        ESP_LOGE(TAG, "找不到 OPF（package document）");
        rd_epub_close();
        return false;
    }

    static char s_opf_dir[192];
    {
        const char *slash = strrchr(opf, '/');
        s_opf_dir[0] = 0;
        if (slash) {
            const size_t dl = (size_t)(slash - opf) + 1;
            if (dl < sizeof(s_opf_dir)) {
                memcpy(s_opf_dir, opf, dl);
                s_opf_dir[dl] = 0;
            }
        }
    }

    const int oi = ep_find(opf);
    if (oi < 0) {
        ESP_LOGE(TAG, "OPF 不在中央目录里：%s", opf);
        rd_epub_close();
        return false;
    }
    ESP_LOGI(TAG, "《%s》%d 字节，OPF = %s（目录 %s）",
             s_title, (int)s_fsize, opf, s_opf_dir[0] ? s_opf_dir : "(根)");

    char (*ids)[EP_ID_LEN] = heap_caps_malloc(sizeof(char) * EP_ID_LEN * EP_MAX_ID,
                                              MALLOC_CAP_8BIT);
    if (!ids) {
        ESP_LOGE(TAG, "spine id 表分配失败");
        rd_epub_close();
        return false;
    }

    ep_opf_ctx_t O;
    memset(&O, 0, sizeof(O));
    O.ids = ids;
    O.dir = s_opf_dir;

    memset(s_chap, 0, sizeof(s_chap));
    s_nchap = 0;

    for (int pass = 0; pass < 2; pass++) {
        ep_xml_t x;
        memset(&x, 0, sizeof(x));
        x.fn  = opf_tag;
        x.ctx = &O;
        O.mode = pass;
        ep_inflate(oi, xml_out, &x);
    }
    const int nid = O.n;
    heap_caps_free(ids);

    if (nid <= 0) {
        ESP_LOGE(TAG, "OPF 里没有 <itemref>（spine 为空）");
        rd_epub_close();
        return false;
    }

    {
        int w = 0;
        for (int i = 0; i < s_nchap; i++) {
            if (s_chap[i].lho) {
                if (w != i) s_chap[w] = s_chap[i];
                w++;
            }
        }
        s_nchap = w;
    }
    ESP_LOGI(TAG, "spine：%d 个 itemref，解析出 %d 章正文", nid, s_nchap);
    if (s_nchap <= 0) {
        ESP_LOGE(TAG, "一章正文都没解析出来");
        rd_epub_close();
        return false;
    }

    s_npages = 0;
    for (int i = 0; i < s_nchap; i++) {
        s_chap[i].first_page = (uint32_t)s_npages;
        int p = ep_chap_walk(i, NULL, -1);
        if (p < 0) p = 0;
        if (p > 0xFFFF) p = 0xFFFF;
        s_chap[i].pages = (uint16_t)p;
        s_npages += p;
        if (prog) prog(i + 1, s_nchap);
    }

    if (s_npages <= 0) {
        ESP_LOGE(TAG, "整本书没有可取的正文字（%d 章都是空的）", s_nchap);
        rd_epub_close();
        return false;
    }

    free_tables();
    s_open = true;
    s_page = 0;

    ESP_LOGI(TAG, "分页完成：%d 章 / %d 页；最大可分配块 %u",
             s_nchap, s_npages,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

bool rd_epub_is_open(void)  { return s_open; }
const char *rd_epub_title(void) { return s_title; }
const char *rd_epub_file(void)  { return s_file; }
int  rd_epub_bytes(void)    { return (int)s_fsize; }
int  rd_epub_pages(void)    { return s_npages; }
int  rd_epub_page(void)     { return s_page; }

bool rd_epub_goto(int idx)
{
    if (!s_open || idx < 0 || idx >= s_npages) return false;
    s_page = idx;
    return true;
}
bool rd_epub_next(void) { return rd_epub_goto(s_page + 1); }
bool rd_epub_prev(void) { return rd_epub_goto(s_page - 1); }

int rd_epub_percent(void)
{
    if (!s_open || s_npages <= 1) return 0;
    return (int)((int64_t)s_page * 100 / (s_npages - 1));
}

bool rd_epub_load_page(rd_page_t *out)
{
    if (!s_open || !out) return false;
    if (s_page < 0 || s_page >= s_npages) return false;

    int ci = -1, local = 0;
    for (int i = 0; i < s_nchap; i++) {
        if (s_chap[i].pages == 0) continue;
        if ((uint32_t)s_page >= s_chap[i].first_page &&
            (uint32_t)s_page <  s_chap[i].first_page + s_chap[i].pages) {
            ci    = i;
            local = s_page - (int)s_chap[i].first_page;
            break;
        }
    }
    if (ci < 0) {
        ESP_LOGE(TAG, "第 %d 页找不到对应章节（章节表可能不一致）", s_page + 1);
        return false;
    }

    const int pages = ep_chap_walk(ci, out, local);
    if (!out->lines && pages <= local) {
        ESP_LOGW(TAG, "第 %d 页（章 %d 第 %d 页）没取到内容", s_page + 1, ci, local + 1);
    }
    return true;
}

#if EPUB_SELFTEST
extern const uint8_t _binary_epub_test_epub_start[];
extern const uint8_t _binary_epub_test_epub_end[];

static void log_cp_line(const uint32_t *cp, int n, int max)
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

void rd_epub_selftest(void)
{
    ESP_LOGW(TAG, "=== EPUB 自检 ===");

    const uint8_t *blob = _binary_epub_test_epub_start;
    const size_t   len  = (size_t)(_binary_epub_test_epub_end - _binary_epub_test_epub_start);
    ESP_LOGW(TAG, "内嵌测试 EPUB %u 字节", (unsigned)len);

    const char *dst = "/sdcard/novels/EPUB样例书.epub";
    FILE *f = fopen(dst, "wb");
    if (!f) {
        ESP_LOGE(TAG, "写不进 %s（卡没挂载？）", dst);
        return;
    }
    const size_t wr = fwrite(blob, 1, len, f);
    fclose(f);
    if (wr != len) {
        ESP_LOGE(TAG, "只写了 %u/%u 字节", (unsigned)wr, (unsigned)len);
        return;
    }
    ESP_LOGW(TAG, "已写到 %s，开始打开", dst);

    if (!rd_epub_open(dst, NULL)) {
        ESP_LOGE(TAG, "打开失败 —— 见上面的错误行");
        return;
    }
    ESP_LOGW(TAG, "《%s》%d 字节 -> %d 页", rd_epub_title(), rd_epub_bytes(), rd_epub_pages());

    static rd_page_t pg;
    if (rd_epub_goto(0) && rd_epub_load_page(&pg)) {
        ESP_LOGW(TAG, "第 1 页 %d 行：", pg.lines);
        if (pg.lines > 0) log_cp_line(pg.cp[0], pg.n[0], 24);
    } else {
        ESP_LOGE(TAG, "取第 1 页失败");
    }

    const int last = rd_epub_pages() - 1;
    if (rd_epub_goto(last) && rd_epub_load_page(&pg)) {
        ESP_LOGW(TAG, "末页（第 %d 页）%d 行，末行：", last + 1, pg.lines);
        if (pg.lines > 0) log_cp_line(pg.cp[pg.lines - 1], pg.n[pg.lines - 1], 24);
    }

    rd_epub_close();
    ESP_LOGW(TAG, "样例书留在 %s，可在阅读列表里直接点开", dst);
    ESP_LOGW(TAG, "=== EPUB 自检结束 ===");
}
#endif
