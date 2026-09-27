#include "nui_qr.h"

#include "esp_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nui_qr";

#define QR_MAX_VER    6
#define QR_MAX_SIZE  (QR_MAX_VER * 4 + 17)

_Static_assert(QR_MAX_SIZE == 41, "版本上限与矩阵尺寸必须一致");

typedef struct {
    uint8_t ec;
    uint8_t blocks;
    uint8_t data;
} qr_blk_t;

static const qr_blk_t BLK[2][QR_MAX_VER] = {
    {
        { 7, 1,  19}, {10, 1,  34}, {15, 1,  55},
        {20, 1,  80}, {26, 1, 108}, {18, 2,  68},
    },
    {
        {10, 1,  16}, {16, 1,  28}, {26, 1,  44},
        {18, 2,  32}, {24, 2,  43}, {16, 4,  27},
    },
};

#define QR_MAX_TOTALW  172

static uint8_t s_mod[QR_MAX_SIZE][QR_MAX_SIZE];
static uint8_t s_fn [QR_MAX_SIZE][QR_MAX_SIZE];

static int s_size;
static int s_ver;
static int s_ecc;
static int s_mask;

static uint8_t gf_mul(uint8_t x, uint8_t y)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (y & 1) r ^= x;
        const uint8_t hi = x & 0x80u;
        x <<= 1;
        if (hi) x ^= 0x1Du;
        y >>= 1;
    }
    return r;
}

static void rs_divisor(int deg, uint8_t *out)
{
    memset(out, 0, (size_t)deg);
    out[deg - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < deg; i++) {
        for (int j = 0; j < deg; j++) {
            out[j] = gf_mul(out[j], root);
            if (j + 1 < deg) out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02u);
    }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *div,
                         int deg, uint8_t *out)
{
    memset(out, 0, (size_t)deg);
    for (int i = 0; i < len; i++) {
        const uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)(deg - 1));
        out[deg - 1] = 0;
        for (int j = 0; j < deg; j++) out[j] ^= gf_mul(div[j], factor);
    }
}

typedef struct {
    uint8_t *buf;
    int      bits;
} bitbuf_t;

static void bb_put(bitbuf_t *b, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (val & (1u << i)) b->buf[b->bits >> 3] |= (uint8_t)(0x80u >> (b->bits & 7));
        b->bits++;
    }
}

static void set_fn(int r, int c, int dark)
{
    s_mod[r][c] = (uint8_t)(dark ? 1 : 0);
    s_fn [r][c] = 1;
}

static void draw_finder(int cr, int cc)
{
    for (int dr = -4; dr <= 4; dr++) {
        for (int dc = -4; dc <= 4; dc++) {
            const int r = cr + dr, c = cc + dc;
            if (r < 0 || r >= s_size || c < 0 || c >= s_size) continue;
            const int a = abs(dr), b = abs(dc);
            const int m = (a > b) ? a : b;
            set_fn(r, c, (m != 2 && m != 4));
        }
    }
}

static void draw_align(int cr, int cc)
{
    for (int dr = -2; dr <= 2; dr++) {
        for (int dc = -2; dc <= 2; dc++) {
            const int a = abs(dr), b = abs(dc);
            const int m = (a > b) ? a : b;
            set_fn(cr + dr, cc + dc, m != 1);
        }
    }
}

static void draw_function_patterns(void)
{

    for (int i = 0; i < s_size; i++) {
        set_fn(6, i, (i % 2) == 0);
        set_fn(i, 6, (i % 2) == 0);
    }

    draw_finder(3, 3);
    draw_finder(3, s_size - 4);
    draw_finder(s_size - 4, 3);

    if (s_ver >= 2) {

        const int p[2] = { 6, s_size - 7 };
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {

                if ((i == 0 && j == 0) ||
                    (i == 0 && j == 1) ||
                    (i == 1 && j == 0)) continue;
                draw_align(p[i], p[j]);
            }
        }
    }

    set_fn(s_size - 8, 8, 1);
}

static uint32_t format_bits(int ecc, int mask)
{

    const int ecl = (ecc == 0) ? 1 : 0;
    const uint32_t data = (uint32_t)((ecl << 3) | mask);

    uint32_t rem = data;
    for (int i = 0; i < 10; i++) {
        rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    }
    return ((data << 10) | rem) ^ 0x5412u;
}

static void draw_format(int ecc, int mask)
{
    const uint32_t bits = format_bits(ecc, mask);

    for (int i = 0; i < 6; i++) set_fn(i, 8, (bits >> i) & 1);
    set_fn(7, 8, (bits >> 6) & 1);
    set_fn(8, 8, (bits >> 7) & 1);
    set_fn(8, 7, (bits >> 8) & 1);
    for (int i = 9; i < 15; i++) set_fn(8, 14 - i, (bits >> i) & 1);

    for (int i = 0; i < 8; i++) set_fn(8, s_size - 1 - i, (bits >> i) & 1);
    for (int i = 8; i < 15; i++) set_fn(s_size - 15 + i, 8, (bits >> i) & 1);
}

static int mask_bit(int m, int r, int c)
{
    switch (m) {
    case 0: return ((r + c) & 1) == 0;
    case 1: return (r & 1) == 0;
    case 2: return (c % 3) == 0;
    case 3: return ((r + c) % 3) == 0;
    case 4: return ((r / 2) + (c / 3)) % 2 == 0;
    case 5: return ((r * c) % 2 + (r * c) % 3) == 0;
    case 6: return (((r * c) % 2 + (r * c) % 3) & 1) == 0;
    case 7: return (((r + c) % 2 + (r * c) % 3) & 1) == 0;
    default: return 0;
    }
}

static void apply_mask(int m)
{
    for (int r = 0; r < s_size; r++) {
        for (int c = 0; c < s_size; c++) {
            if (!s_fn[r][c] && mask_bit(m, r, c)) s_mod[r][c] ^= 1;
        }
    }
}

#define PEN_N1   3
#define PEN_N2   3
#define PEN_N3  40
#define PEN_N4  10

static int pen_runs(const uint8_t *v, int n)
{
    int score = 0, run = 1;
    for (int i = 1; i <= n; i++) {
        if (i < n && v[i] == v[i - 1]) { run++; continue; }
        if (run >= 5) score += PEN_N1 + (run - 5);
        run = 1;
    }
    return score;
}

static int pen_finder_like(const uint8_t *v, int n)
{
    static const uint8_t P1[11] = { 1,0,1,1,1,0,1,0,0,0,0 };
    static const uint8_t P2[11] = { 0,0,0,0,1,0,1,1,1,0,1 };
    int score = 0;
    for (int i = 0; i + 11 <= n; i++) {
        int m1 = 1, m2 = 1;
        for (int k = 0; k < 11; k++) {
            if (v[i + k] != P1[k]) m1 = 0;
            if (v[i + k] != P2[k]) m2 = 0;
            if (!m1 && !m2) break;
        }
        if (m1) score += PEN_N3;
        if (m2) score += PEN_N3;
    }
    return score;
}

static int penalty(void)
{
    static uint8_t line[QR_MAX_SIZE];
    int score = 0, dark = 0;

    for (int r = 0; r < s_size; r++) {
        for (int c = 0; c < s_size; c++) {
            line[c] = s_mod[r][c];
            dark += s_mod[r][c];
        }
        score += pen_runs(line, s_size);
        score += pen_finder_like(line, s_size);
    }
    for (int c = 0; c < s_size; c++) {
        for (int r = 0; r < s_size; r++) line[r] = s_mod[r][c];
        score += pen_runs(line, s_size);
        score += pen_finder_like(line, s_size);
    }
    for (int r = 0; r < s_size - 1; r++) {
        for (int c = 0; c < s_size - 1; c++) {
            const uint8_t v = s_mod[r][c];
            if (v == s_mod[r][c + 1] && v == s_mod[r + 1][c] && v == s_mod[r + 1][c + 1]) {
                score += PEN_N2;
            }
        }
    }

    const int total = s_size * s_size;
    const int k = (abs(dark * 20 - total * 10) + total - 1) / total - 1;
    score += k * PEN_N4;

    return score;
}

static int capacity_bytes(int ver, int ecc)
{
    const qr_blk_t *b = &BLK[ecc][ver - 1];
    const int data_cw = b->data * b->blocks;
    return (data_cw * 8 - 4 - 8) / 8;
}

static int build_codewords(const uint8_t *text, int len, uint8_t *out)
{
    const qr_blk_t *b = &BLK[s_ecc][s_ver - 1];
    const int data_cw  = b->data * b->blocks;
    const int total_cw = b->data * b->blocks + b->ec * b->blocks;

    memset(out, 0, (size_t)total_cw);

    bitbuf_t bb = { .buf = out, .bits = 0 };
    bb_put(&bb, 0x4, 4);
    bb_put(&bb, (uint32_t)len, 8);
    for (int i = 0; i < len; i++) bb_put(&bb, text[i], 8);

    const int room = data_cw * 8 - bb.bits;
    bb_put(&bb, 0, room < 4 ? room : 4);

    if (bb.bits % 8) bb_put(&bb, 0, 8 - (bb.bits % 8));

    for (int i = bb.bits / 8, alt = 0; i < data_cw; i++, alt++) {
        out[i] = (alt & 1) ? 0x11u : 0xECu;
    }

    return total_cw;
}

static void interleave(uint8_t *out, int total_cw)
{
    const qr_blk_t *b = &BLK[s_ecc][s_ver - 1];
    const int nblk = b->blocks;
    const int dlen = b->data;
    const int elen = b->ec;

    static uint8_t ecbuf[QR_MAX_TOTALW];
    static uint8_t div[64];
    rs_divisor(elen, div);

    for (int blk = 0; blk < nblk; blk++) {
        rs_remainder(out + blk * dlen, dlen, div, elen, ecbuf + blk * elen);
    }

    static uint8_t tmp[QR_MAX_TOTALW];
    int k = 0;
    for (int i = 0; i < dlen; i++) {
        for (int blk = 0; blk < nblk; blk++) tmp[k++] = out[blk * dlen + i];
    }
    for (int i = 0; i < elen; i++) {
        for (int blk = 0; blk < nblk; blk++) tmp[k++] = ecbuf[blk * elen + i];
    }
    memcpy(out, tmp, (size_t)total_cw);
}

static void place_codewords(const uint8_t *cw, int total_cw)
{
    const int nbits = total_cw * 8;
    int i = 0;

    for (int right = s_size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < s_size; vert++) {
            for (int j = 0; j < 2; j++) {
                const int c = right - j;
                const int up = ((right + 1) & 2) == 0;
                const int r  = up ? (s_size - 1 - vert) : vert;
                if (s_fn[r][c] || i >= nbits) continue;
                s_mod[r][c] = (uint8_t)((cw[i >> 3] >> (7 - (i & 7))) & 1);
                i++;
            }
        }
    }
}

int nui_qr_encode(const char *text)
{
    s_size = 0;
    s_mask = 0;
    s_ecc  = 0;

    if (!text) return 0;
    const size_t len = strlen(text);
    if (len == 0 || len > 255) {
        ESP_LOGE(TAG, "内容长度 %u 不支持（1..255 字节）", (unsigned)len);
        return 0;
    }

    int ver = 0, ecc = 0;
    for (int e = 1; e >= 0 && !ver; e--) {
        for (int v = 1; v <= QR_MAX_VER; v++) {
            if ((int)len <= capacity_bytes(v, e)) { ver = v; ecc = e; break; }
        }
    }
    if (!ver) {
        ESP_LOGE(TAG, "内容太长（%u 字节），超出版本 %d 的容量", (unsigned)len, QR_MAX_VER);
        return 0;
    }

    s_ver  = ver;
    s_ecc  = ecc;
    s_size = ver * 4 + 17;

    static uint8_t cw[QR_MAX_TOTALW];

    const int total_cw = build_codewords((const uint8_t *)text, (int)len, cw);
    interleave(cw, total_cw);

    memset(s_mod, 0, sizeof(s_mod));
    memset(s_fn,  0, sizeof(s_fn));
    draw_function_patterns();
    draw_format(ecc, 0);

    place_codewords(cw, total_cw);

    int best = 0, best_pen = -1;
    for (int m = 0; m < 8; m++) {
        apply_mask(m);
        draw_format(ecc, m);
        const int p = penalty();
        if (best_pen < 0 || p < best_pen) { best_pen = p; best = m; }
        apply_mask(m);
    }
    apply_mask(best);
    draw_format(ecc, best);
    s_mask = best;

    ESP_LOGI(TAG, "二维码：%u 字节 -> v%d / %s / 掩码 %d / %dx%d 模块（罚分 %d）",
             (unsigned)len, ver, ecc ? "M" : "L", best, s_size, s_size, best_pen);
    return s_size;
}

int nui_qr_size(void) { return s_size; }

int nui_qr_mask(void) { return s_mask; }

int nui_qr_ecc(void)  { return s_ecc; }

bool nui_qr_dark(int row, int col)
{
    if (row < 0 || col < 0 || row >= s_size || col >= s_size) return false;
    return s_mod[row][col] != 0;
}
