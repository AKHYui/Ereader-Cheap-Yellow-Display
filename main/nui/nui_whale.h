#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUI_WHALE_N      120
#define NUI_WHALE_SCALE    2
#define NUI_WHALE_FRAMES   2

void nui_whale_row(uint16_t *row, int sy, int ox, int oy, int frame);

uint16_t nui_whale_cyan(void);

uint16_t nui_whale_deep(void);

uint16_t nui_whale_light(void);

#ifdef __cplusplus
}
#endif
