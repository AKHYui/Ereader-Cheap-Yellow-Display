#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PV_IMG_OK = 0,
    PV_IMG_ERR_OPEN,
    PV_IMG_ERR_FORMAT,
    PV_IMG_ERR_DECODE,
    PV_IMG_ERR_NOMEM,
} pv_img_err_t;

pv_img_err_t pv_jpeg_show(const char *path);

pv_img_err_t pv_jpeg_show_ram(const void *data, unsigned len);

const char *pv_img_err_text(pv_img_err_t e);

#ifdef __cplusplus
}
#endif
