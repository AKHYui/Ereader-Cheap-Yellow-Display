#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pv_disp_init(esp_lcd_panel_handle_t panel,
                       esp_lcd_panel_io_handle_t io);
void      pv_disp_deinit(void);

void pv_disp_img_clear(void);

void pv_disp_img_begin(int src_w, int src_h, int dst_w, int dst_h);

void pv_disp_img_block(int x, int y, int w, int h,
                       const uint16_t *px, int stride);

void pv_disp_img_end(void);

void pv_disp_set_swap(bool on);
bool pv_disp_get_swap(void);

void pv_disp_colortest(void);

void      pv_disp_page_begin(uint16_t bg);
uint16_t *pv_disp_page_row(int y);
void      pv_disp_page_end(void);

#define PV_PRESS_NONE    0
#define PV_PRESS_LEFT  (-1)
#define PV_PRESS_RIGHT   1
#define PV_PRESS_HOME    2
#define PV_PRESS_DEL     3

#define PV_PRESS_DEL_BAD 4

void pv_disp_bar(int idx, int total, int press);

void pv_disp_btn_rect(int which, int *x, int *y, int *w, int *h);

void pv_disp_home_rect(int *x, int *y, int *w, int *h);

void pv_disp_del_rect(int *x, int *y, int *w, int *h);

#ifdef __cplusplus
}
#endif
