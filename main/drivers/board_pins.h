#pragma once

#define PIN_LCD_MOSI     13
#define PIN_LCD_MISO     12
#define PIN_LCD_SCLK     14
#define PIN_LCD_CS       15
#define PIN_LCD_DC        2
#define PIN_LCD_RST      -1
#define PIN_LCD_BL       21

#define LCD_H_RES        240
#define LCD_V_RES        320
#define LCD_SPI_HOST     SPI2_HOST
#define LCD_PIXEL_CLOCK  (40 * 1000 * 1000)

#define PIN_TOUCH_MOSI   32
#define PIN_TOUCH_MISO   39
#define PIN_TOUCH_SCLK   25
#define PIN_TOUCH_CS     33
#define PIN_TOUCH_IRQ    36

#define PIN_SD_MOSI      23
#define PIN_SD_MISO      19
#define PIN_SD_SCLK      18
#define PIN_SD_CS         5
#define SD_SPI_HOST      SPI3_HOST

#define PIN_RGB_R         4
#define PIN_RGB_G        16
#define PIN_RGB_B        17
#define PIN_LDR_ADC      34
#define PIN_SPEAKER      26
#define PIN_BOOT_BTN      0

#define LCD_SWAP_XY      0
#define LCD_MIRROR_X     1
#define LCD_MIRROR_Y     0

#define LCD_INVERT_COLOR 0

#define LCD_BGR_ORDER    1
