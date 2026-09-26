#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const uint8_t *nui_glyph_find(uint16_t cp);

int nui_glyph_w(void);
int nui_glyph_h(void);

#ifdef __cplusplus
}
#endif
