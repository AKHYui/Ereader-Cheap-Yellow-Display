#pragma once

#include <stdbool.h>
#include "pv_jpeg.h"

#ifdef __cplusplus
extern "C" {
#endif

pv_img_err_t pv_bmp_show(const char *path);

bool pv_bmp_probe(const char *path);

#ifdef __cplusplus
}
#endif
