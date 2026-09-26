#pragma once
#include <stdbool.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_lcd_panel_handle_t lcd_st7789_init(void);

esp_lcd_panel_io_handle_t lcd_st7789_io(void);

esp_lcd_panel_handle_t lcd_st7789_panel(void);

void lcd_set_backlight(bool on);

void lcd_set_brightness(int pct);
int  lcd_get_brightness(void);

void lcd_bl_save(void);

bool lcd_get_bgr_order(void);
void lcd_set_bgr_order(bool bgr);

bool lcd_get_invert(void);
void lcd_set_invert(bool invert);

void lcd_cfg_load(void);
void lcd_cfg_save(bool bgr, bool invert);

void lcd_cfg_reset(void);

#ifdef __cplusplus
}
#endif
