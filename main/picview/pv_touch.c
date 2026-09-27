#include "pv_touch.h"
#include "pv_config.h"
#include "board_pins.h"

#include "nui_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = PV_TAG;

#define TOUCH_POLL_MS          10
#define TOUCH_Z_PRESS         500
#define TOUCH_Z_RELEASE       250
#define TOUCH_CONFIRM           2

#define TOUCH_SETTLE            2
#define TOUCH_BASE_RELEARN_N  300

#define TOUCH_RAW_MIN         250
#define TOUCH_RAW_MAX        3750

#define TOUCH_Q_DEPTH           8

static QueueHandle_t s_q;
static volatile bool s_ready   = false;
static volatile bool s_pressed = false;

static uint8_t  s_confirm_cnt  = 0;
static uint32_t s_idle_cnt     = 0;
static int      s_base_z       = 0;

static volatile int  s_last_x     = 0;
static volatile int  s_last_y     = 0;
static volatile bool s_last_valid = false;
static int           s_settle     = 0;

static void touch_spi_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_CS) | (1ULL << PIN_TOUCH_SCLK) |
                        (1ULL << PIN_TOUCH_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_MISO) | (1ULL << PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    gpio_set_level(PIN_TOUCH_CS, 1);
    gpio_set_level(PIN_TOUCH_SCLK, 0);
}

static inline void touch_delay(void)
{
    esp_rom_delay_us(1);
}

#define XPT_CMD_X   0xD0
#define XPT_CMD_Y   0x90
#define XPT_CMD_Z1  0xB1
#define XPT_CMD_Z2  0xC1

static uint16_t xpt_read(uint8_t cmd)
{
    gpio_set_level(PIN_TOUCH_CS, 0);
    touch_delay();

    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_TOUCH_SCLK, 0);
        gpio_set_level(PIN_TOUCH_MOSI, (cmd >> i) & 1);
        touch_delay();
        gpio_set_level(PIN_TOUCH_SCLK, 1);
        touch_delay();
    }

    uint16_t val = 0;
    for (int i = 0; i < 13; i++) {
        gpio_set_level(PIN_TOUCH_SCLK, 0);
        touch_delay();
        const int bit = gpio_get_level(PIN_TOUCH_MISO);
        gpio_set_level(PIN_TOUCH_SCLK, 1);
        touch_delay();

        if (i == 0) continue;
        val = (uint16_t)((val << 1) | bit);
    }

    gpio_set_level(PIN_TOUCH_CS, 1);
    touch_delay();
    return val;
}

static void xpt_read_xy(uint16_t *x_out, uint16_t *y_out)
{
    uint16_t xs[5], ys[5];
    for (int i = 0; i < 5; i++) {
        xs[i] = xpt_read(XPT_CMD_X);
        ys[i] = xpt_read(XPT_CMD_Y);
    }
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (xs[j] < xs[i]) { const uint16_t t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
            if (ys[j] < ys[i]) { const uint16_t t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
        }
    }
    *x_out = xs[2];
    *y_out = ys[2];
}

static int xpt_pressure(void)
{
    const int z1 = (int)xpt_read(XPT_CMD_Z1);
    const int z2 = (int)xpt_read(XPT_CMD_Z2);
    return z1 + 4095 - z2;
}

static bool touch_sample(int *x_out, int *y_out, int *z_out,
                         int *rx_out, int *ry_out)
{
    uint16_t rx = 0, ry = 0;
    xpt_read_xy(&rx, &ry);
    *z_out = xpt_pressure();

    if (rx_out) *rx_out = (int)rx;
    if (ry_out) *ry_out = (int)ry;

    const bool raw_ok = (rx >= TOUCH_RAW_MIN && rx <= TOUCH_RAW_MAX &&
                         ry >= TOUCH_RAW_MIN && ry <= TOUCH_RAW_MAX);

    int32_t sx = (int32_t)(rx - TOUCH_RAW_MIN) * LCD_H_RES
               / (TOUCH_RAW_MAX - TOUCH_RAW_MIN);
    int32_t sy = (int32_t)(ry - TOUCH_RAW_MIN) * LCD_V_RES
               / (TOUCH_RAW_MAX - TOUCH_RAW_MIN);

#if LCD_MIRROR_X
    sx = LCD_H_RES - 1 - sx;
#endif
#if LCD_MIRROR_Y
    sy = LCD_V_RES - 1 - sy;
#endif

    if (sx < 0) sx = 0;
    if (sx >= LCD_H_RES) sx = LCD_H_RES - 1;
    if (sy < 0) sy = 0;
    if (sy >= LCD_V_RES) sy = LCD_V_RES - 1;

    *x_out = (int)sx;
    *y_out = (int)sy;
    return raw_ok;
}

static void touch_probe(void)
{
    int z[7];
    for (int i = 0; i < 7; i++) z[i] = xpt_pressure();
    for (int i = 0; i < 6; i++) {
        for (int j = i + 1; j < 7; j++) {
            if (z[j] < z[i]) { const int t = z[i]; z[i] = z[j]; z[j] = t; }
        }
    }
    s_base_z = z[3];
    ESP_LOGI(TAG, "触摸基线 base_z=%d (采样 %d..%d) irq=%d",
             s_base_z, z[0], z[6], gpio_get_level(PIN_TOUCH_IRQ));
}

static void pv_touch_task(void *arg)
{
    (void)arg;

    for (;;) {
        int x = 0, y = 0, z_raw = 0, rx = 0, ry = 0;
        const bool raw_ok = touch_sample(&x, &y, &z_raw, &rx, &ry);

        const int  dz  = z_raw - s_base_z;

        const int  thr = s_pressed ? TOUCH_Z_RELEASE : TOUCH_Z_PRESS;
        const bool now = dz > thr;

        if (s_pressed && now && raw_ok) {
            if (s_settle < TOUCH_SETTLE) {
                s_settle++;
            } else {
                s_last_x     = x;
                s_last_y     = y;
                s_last_valid = true;
            }
        }

        if (now != s_pressed) {
            if (++s_confirm_cnt >= TOUCH_CONFIRM) {
                s_pressed     = now;
                s_confirm_cnt = 0;

                if (now) {
                    s_last_valid = false;
                    s_settle     = 0;

                    nui_led_flash(NUI_LED_WHITE);
                }

                const pv_touch_evt_t ev = { x, y, rx, ry, z_raw, now };
                if (s_q) {

                    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
                        pv_touch_evt_t drop;
                        (void)xQueueReceive(s_q, &drop, 0);
                        (void)xQueueSend(s_q, &ev, 0);
                    }
                }
            }
        } else {
            s_confirm_cnt = 0;
        }

        if (s_pressed) {
            s_idle_cnt = 0;
        } else if (++s_idle_cnt >= TOUCH_BASE_RELEARN_N) {
            s_base_z  += (z_raw - s_base_z) / 16;
            s_idle_cnt = TOUCH_BASE_RELEARN_N;
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

void pv_touch_start(void)
{
    if (s_ready) return;

    s_q = xQueueCreate(TOUCH_Q_DEPTH, sizeof(pv_touch_evt_t));
    if (!s_q) {
        ESP_LOGE(TAG, "触摸事件队列创建失败");
        return;
    }

    touch_spi_init();
    touch_probe();

    if (xTaskCreate(pv_touch_task, "pv_touch", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "触摸任务创建失败");
        vQueueDelete(s_q);
        s_q = NULL;
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "触摸就绪（独立任务，%dms 周期）", TOUCH_POLL_MS);
}

bool pv_touch_ready(void)
{
    return s_ready;
}

bool pv_touch_take(pv_touch_evt_t *evt)
{
    if (!s_q || !evt) return false;
    return xQueueReceive(s_q, evt, 0) == pdTRUE;
}

bool pv_touch_last(int *x, int *y)
{
    if (!s_last_valid) return false;
    if (x) *x = s_last_x;
    if (y) *y = s_last_y;
    return true;
}

int pv_touch_hit(int (*hit)(int x, int y, void *ctx), void *ctx)
{
    if (!hit) return 0;
    int x = 0, y = 0;
    if (!pv_touch_last(&x, &y)) return 0;
    return hit(x, y, ctx);
}

bool pv_touch_is_down(void)
{
    return s_pressed;
}
