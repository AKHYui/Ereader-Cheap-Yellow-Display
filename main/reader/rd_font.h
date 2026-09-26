#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RD_CELL_W     16
#define RD_CELL_H     16
#define RD_ADV_HAN    16
#define RD_ADV_ASC     8

typedef struct {
    const uint8_t *bm;
    int            row_bytes;
    int            bpp;
} rd_glyph_t;

bool rd_font_ok(void);

bool rd_font_glyph(uint32_t cp, rd_glyph_t *out);

bool rd_font_has(uint32_t cp);

static inline int rd_font_adv(uint32_t cp)
{
    return (cp < 0x2E80u) ? RD_ADV_ASC : RD_ADV_HAN;
}

int rd_font_width(const uint32_t *cp, int n);

void rd_font_row(uint16_t *row, int sy, int x, int y_top,
                 const uint32_t *cp, int n, uint16_t fg, uint16_t bg);

int  rd_font_ascii_w(const char *s);

void rd_font_ascii_row(uint16_t *row, int sy, int x, int y_top, const char *s,
                       uint16_t fg, uint16_t bg);

void rd_font_ascii_mid(uint16_t *row, int sy, int cx, int y_top, const char *s,
                       uint16_t fg, uint16_t bg);

int rd_cp_from_utf8(const char *s, uint32_t *out, int max);

int rd_cp_fit(const uint32_t *cp, int n, int max_w);

#ifdef __cplusplus
}
#endif
