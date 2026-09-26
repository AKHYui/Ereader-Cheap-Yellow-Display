#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUI_ASC_FIRST   0x20
#define NUI_ASC_LAST    0x7E
#define NUI_ASC_COUNT   (NUI_ASC_LAST - NUI_ASC_FIRST + 1)

const uint8_t *nui_ascii_find(uint8_t ch);

int nui_ascii_w(void);
int nui_ascii_h(void);

#ifdef __cplusplus
}
#endif
