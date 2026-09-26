#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUI_BG        PV_BG
#define NUI_PANEL     PV_BAR_BG
#define NUI_EDGE      PV_BTN_BG
#define NUI_FG        PV_BTN_FG
#define NUI_DIM       0x8410
#define NUI_PRESS     PV_BTN_PRESS
#define NUI_OK        0x07E0
#define NUI_WARN      0xFD20
#define NUI_ERR       0xF800

void nui_hspan(uint16_t *row, int x0, int x1, uint16_t c);

void nui_putpx(uint16_t *row, int x, uint16_t c);

uint16_t nui_blend(uint16_t fg, uint16_t bg, int lv);

uint16_t nui_blend_lv(uint16_t fg, uint16_t bg, int lv, int max_lv);

void nui_rect_row(uint16_t *row, int sy, int x, int y, int w, int h, uint16_t c);

void nui_frame_row(uint16_t *row, int sy, int x, int y, int w, int h,
                   uint16_t fill, uint16_t edge, int t);

uint16_t nui_utf8_next(const char **p);

int nui_text_w(const char *s);

int nui_text_h(void);

void nui_text_at(uint16_t *row, int sy, int x, int y_top, const char *s,
                 uint16_t fg, uint16_t bg);

void nui_text_center(uint16_t *row, int sy, int cx, int y_top, const char *s,
                     uint16_t fg, uint16_t bg);

void nui_text_mid(uint16_t *row, int sy, int x, int y, int h, const char *s,
                  uint16_t fg, uint16_t bg);

void nui_text_mid_center(uint16_t *row, int sy, int cx, int y, int h,
                         const char *s, uint16_t fg, uint16_t bg);

int  nui_ascii2x_w(const char *s);

void nui_ascii2x_row(uint16_t *row, int sy, int x, int y_top, const char *s,
                     uint16_t fg);

void nui_tri_row(uint16_t *row, int sy, int cx, int cy, int size, int dir,
                 uint16_t c);

void nui_sig_row(uint16_t *row, int sy, int x, int y, int level, uint16_t c);

void nui_lock_row(uint16_t *row, int sy, int x, int y, uint16_t c);

void nui_arrow_row(uint16_t *row, int sy, int x, int y, uint16_t c);

#define NUI_ITEM_X      14
#define NUI_ITEM_W      (PV_SCR_W - NUI_ITEM_X * 2)
#define NUI_ITEM_H      56
#define NUI_ITEM_GAP    12
#define NUI_ITEM_Y0     58

#define NUI_TITLE_Y      4
#define NUI_TITLE_H     40
#define NUI_TITLE_LINE  46

int nui_item_y(int i);

void nui_title_row(uint16_t *row, int sy, const char *title);

uint16_t nui_item_row(uint16_t *row, int sy, int i, bool pressed);

#define NUI_BAR_Y        272
#define NUI_BAR_H         48

void nui_button_row(uint16_t *row, int sy, int x, int y, int w, int h,
                    const char *label, bool pressed);

#ifdef __cplusplus
}
#endif
