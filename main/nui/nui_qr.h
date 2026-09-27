#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int nui_qr_encode(const char *text);

int nui_qr_size(void);

bool nui_qr_dark(int row, int col);

int nui_qr_mask(void);
int nui_qr_ecc(void);

#ifdef __cplusplus
}
#endif
