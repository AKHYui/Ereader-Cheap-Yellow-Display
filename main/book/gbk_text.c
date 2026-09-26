#include "gbk_text.h"
#include "gbk_map.h"

static int utf8_put(char *buf, int buf_len, int idx, uint32_t cp)
{
    if (cp < 0x80) {
        if (idx + 1 >= buf_len) return -1;
        buf[idx++] = (char)cp;
    } else if (cp < 0x800) {
        if (idx + 2 >= buf_len) return -1;
        buf[idx++] = (char)(0xC0 | (cp >> 6));
        buf[idx++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (idx + 3 >= buf_len) return -1;
        buf[idx++] = (char)(0xE0 | (cp >> 12));
        buf[idx++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[idx++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (idx + 4 >= buf_len) return -1;
        buf[idx++] = (char)(0xF0 | (cp >> 18));
        buf[idx++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[idx++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[idx++] = (char)(0x80 | (cp & 0x3F));
    }
    return idx;
}

static uint32_t gbk_pair_to_unicode(uint8_t b1, uint8_t b2)
{
    const uint32_t cp = gbk_lookup(b1, b2);
    return cp ? cp : 0xFFFD;
}

int gbk_to_utf8(const char *src, char *dst, int dst_len)
{
    if (!dst || dst_len <= 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }

    const uint8_t *p = (const uint8_t *)src;
    int o = 0;

    while (*p) {
        uint32_t cp;

        if (*p < 0x80) {
            cp = *p++;
        } else if (p[1] == 0) {
            cp = 0xFFFD;
            p++;
        } else {
            cp = gbk_pair_to_unicode(p[0], p[1]);
            p += 2;
        }

        const int n = utf8_put(dst, dst_len, o, cp);
        if (n < 0) break;
        o = n;
    }

    dst[o] = '\0';
    return o;
}
