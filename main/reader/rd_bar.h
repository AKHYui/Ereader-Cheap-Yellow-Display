#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_BAR_Y        272
#define RD_BAR_H        (PV_SCR_H - RD_BAR_Y)
#define RD_BTN_W         55
#define RD_BTN_H         44
#define RD_BTN_GAP        4
#define RD_BAR_MAX        4

int rd_bar_x(int i, int n);

int rd_bar_y(void);

void rd_bar_row(uint16_t *row, int sy, int n, const char *const *labels, int pressed);

int rd_bar_hit(int x, int y, int n);

#ifdef __cplusplus
}
#endif
