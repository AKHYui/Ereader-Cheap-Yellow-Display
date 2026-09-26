#include "lcd_st7789.h"
#include "board_pins.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "lcd";

static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

#define MADCTL_MY   0x80
#define MADCTL_MX   0x40
#define MADCTL_MV   0x20
#define MADCTL_BGR  0x08
#define LCD_CMD_MADCTL 0x36

static bool s_bgr = (LCD_BGR_ORDER != 0);
static bool s_inv = (LCD_INVERT_COLOR != 0);

static esp_err_t madctl_apply(void)
{
    uint8_t v = 0;
    if (LCD_MIRROR_Y) v |= MADCTL_MY;
    if (LCD_MIRROR_X) v |= MADCTL_MX;
#if LCD_SWAP_XY
    v |= MADCTL_MV;
#endif
    if (s_bgr) v |= MADCTL_BGR;
    return esp_lcd_panel_io_tx_param(s_io, LCD_CMD_MADCTL, &v, 1);
}

static esp_err_t inv_apply(void)
{
    return esp_lcd_panel_invert_color(s_panel, s_inv);
}

#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_FREQ    5000
#define BL_DUTY_MAX     1023

#define BL_PCT_MIN      1
#define BL_PCT_MAX      100
#define BL_PCT_DEFAULT  100

static int s_bl_pct = BL_PCT_DEFAULT;

static uint32_t bl_duty_of(int pct)
{
    if (pct < BL_PCT_MIN) pct = BL_PCT_MIN;
    if (pct > BL_PCT_MAX) pct = BL_PCT_MAX;

    return (uint32_t)((BL_DUTY_MAX * (uint32_t)pct + 50) / 100);
}

static void bl_hw_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    const ledc_channel_config_t c = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void lcd_set_brightness(int pct)
{
    if (pct < BL_PCT_MIN) pct = BL_PCT_MIN;
    if (pct > BL_PCT_MAX) pct = BL_PCT_MAX;
    s_bl_pct = pct;

    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, bl_duty_of(pct));
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

int lcd_get_brightness(void)
{
    return s_bl_pct;
}

void lcd_set_backlight(bool on)
{
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, on ? bl_duty_of(s_bl_pct) : 0);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_lcd_panel_handle_t lcd_st7789_init(void)
{

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &s_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR
                                       : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));

    ESP_ERROR_CHECK(inv_apply());

#if LCD_SWAP_XY
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel,
                                         LCD_MIRROR_X ? true : false,
                                         LCD_MIRROR_Y ? true : false));

    ESP_ERROR_CHECK(madctl_apply());

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    bl_hw_init();
    lcd_set_brightness(BL_PCT_DEFAULT);

    ESP_LOGI(TAG, "ST7789 %dx%d ready (invert=%d swap=%d bgr=%d) 背光 PWM %dHz/%d档",
             LCD_H_RES, LCD_V_RES,
             (int)s_inv, LCD_SWAP_XY, (int)s_bgr,
             BL_LEDC_FREQ, BL_PCT_MAX);
    return s_panel;
}

bool lcd_get_bgr_order(void)
{
    return s_bgr;
}

void lcd_set_bgr_order(bool bgr)
{
    if (bgr == s_bgr) return;
    s_bgr = bgr;

    const esp_err_t r = madctl_apply();

    ESP_LOGW(TAG, "MADCTL 元素顺序 → %s（调用方需整屏重绘）rc=%s",
             bgr ? "BGR" : "RGB", esp_err_to_name(r));
}

bool lcd_get_invert(void)
{
    return s_inv;
}

void lcd_set_invert(bool invert)
{
    if (invert == s_inv) return;
    s_inv = invert;

    const esp_err_t r = inv_apply();

    ESP_LOGW(TAG, "面板反色 → %s（调用方需整屏重绘）rc=%s",
             invert ? "开" : "关", esp_err_to_name(r));
}

#define PC_NVS_NS  "panelcfg"
#define PC_KEY_BGR "bgr"
#define PC_KEY_INV "inv"
#define PC_KEY_BL  "bl"

void lcd_cfg_load(void)
{
    uint8_t b = 0, i = 0, bl = 0;
    bool has_b = false, has_i = false, has_bl = false;

    nvs_handle_t h;
    if (nvs_open(PC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        has_b  = (nvs_get_u8(h, PC_KEY_BGR, &b) == ESP_OK);
        has_i  = (nvs_get_u8(h, PC_KEY_INV, &i) == ESP_OK);
        has_bl = (nvs_get_u8(h, PC_KEY_BL, &bl) == ESP_OK);
        nvs_close(h);
    }

    s_bgr = has_b ? (b != 0) : (LCD_BGR_ORDER != 0);
    s_inv = has_i ? (i != 0) : (LCD_INVERT_COLOR != 0);

    const esp_err_t r1 = madctl_apply();
    const esp_err_t r2 = inv_apply();

    if (has_bl) lcd_set_brightness((int)bl);

    ESP_LOGW(TAG, "面板参数生效: bgr=%d(%s) inv=%d(%s) 背光=%d%%(%s) rc=%s/%s",
             (int)s_bgr, has_b ? "NVS覆盖" : "宏",
             (int)s_inv, has_i ? "NVS覆盖" : "宏",
             s_bl_pct, has_bl ? "NVS" : "默认",
             esp_err_to_name(r1), esp_err_to_name(r2));
}

void lcd_bl_save(void)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(PC_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "亮度存 NVS 失败（打开命名空间）: %s", esp_err_to_name(r));
        return;
    }
    r = nvs_set_u8(h, PC_KEY_BL, (uint8_t)s_bl_pct);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "背光亮度已存 NVS: %d%% rc=%s（下次开机沿用）",
             s_bl_pct, esp_err_to_name(r));
}

void lcd_cfg_save(bool bgr, bool invert)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(PC_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "面板参数存 NVS 失败（打开命名空间）: %s", esp_err_to_name(r));
        return;
    }
    nvs_set_u8(h, PC_KEY_BGR, bgr ? 1 : 0);
    nvs_set_u8(h, PC_KEY_INV, invert ? 1 : 0);
    r = nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "面板参数已存 NVS: bgr=%d inv=%d rc=%s（下次开机即用）",
             (int)bgr, (int)invert, esp_err_to_name(r));
}

void lcd_cfg_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(PC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, PC_KEY_BGR);
        nvs_erase_key(h, PC_KEY_INV);
        nvs_erase_key(h, PC_KEY_BL);
        nvs_commit(h);
        nvs_close(h);
    }
    lcd_set_bgr_order(LCD_BGR_ORDER != 0);
    lcd_set_invert(LCD_INVERT_COLOR != 0);
    lcd_set_brightness(BL_PCT_DEFAULT);
    ESP_LOGW(TAG, "面板参数已回到宏默认: bgr=%d inv=%d 背光=%d%%",
             (LCD_BGR_ORDER != 0), (LCD_INVERT_COLOR != 0), BL_PCT_DEFAULT);
}

esp_lcd_panel_io_handle_t lcd_st7789_io(void)
{
    return s_io;
}

esp_lcd_panel_handle_t lcd_st7789_panel(void)
{
    return s_panel;
}
