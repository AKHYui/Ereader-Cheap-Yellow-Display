#include "nui_led.h"

#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "nui_led";

#define LED_NS      "ui"
#define LED_KEY     "led"

#define BLINK_MS    200

static bool               s_en = true;
static esp_timer_handle_t s_timer;
static nui_led_state_t    s_color;
static int                s_left;
static bool               s_lit;

static void apply_color(nui_led_state_t st)
{
    bool r = false, g = false, b = false;

    switch (st) {
    case NUI_LED_WHITE: r = true;  g = true;  b = true;  break;
    case NUI_LED_OK:    g = true;                        break;
    case NUI_LED_BUSY: b = true;                         break;
    case NUI_LED_WARN: r = true;  g = true;              break;
    case NUI_LED_ERR:  r = true;                         break;
    default:                                             break;
    }

    gpio_set_level(PIN_RGB_R, r ? 0 : 1);
    gpio_set_level(PIN_RGB_G, g ? 0 : 1);
    gpio_set_level(PIN_RGB_B, b ? 0 : 1);
}

static void apply_off(void)
{
    gpio_set_level(PIN_RGB_R, 1);
    gpio_set_level(PIN_RGB_G, 1);
    gpio_set_level(PIN_RGB_B, 1);
}

static void blink_cb(void *arg)
{
    (void)arg;

    if (!s_en) {
        esp_timer_stop(s_timer);
        apply_off();
        s_lit = false;
        return;
    }

    if (!s_lit) {
        s_lit = true;
        apply_color(s_color);
        return;
    }

    s_lit = false;
    apply_off();

    if (s_left > 0 && --s_left == 0) {
        esp_timer_stop(s_timer);
    }
}

void nui_led_init(void)
{
    const int pins[3] = { PIN_RGB_R, PIN_RGB_G, PIN_RGB_B };
    for (int i = 0; i < 3; i++) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&c);
    }
    apply_off();

    s_en = true;
    nvs_handle_t h;
    if (nvs_open(LED_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 1;
        if (nvs_get_u8(h, LED_KEY, &v) == ESP_OK) s_en = (v != 0);
        nvs_close(h);
    }

    const esp_timer_create_args_t ta = {
        .callback = blink_cb,
        .name     = "led",
    };
    if (esp_timer_create(&ta, &s_timer) != ESP_OK) {
        s_timer = NULL;
        ESP_LOGE(TAG, "定时器创建失败，指示灯不可用");
    }

    ESP_LOGI(TAG, "RGB 指示灯就绪（%s）", s_en ? "开" : "关");

    if (s_en) nui_led_flash(NUI_LED_OK);
}

bool nui_led_enabled(void) { return s_en; }

void nui_led_set_enabled(bool on)
{
    s_en = on;
    if (!on) nui_led_stop();
    ESP_LOGI(TAG, "指示灯开关：%s", on ? "开" : "关");

    nvs_handle_t h;
    if (nvs_open(LED_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, LED_KEY, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void nui_led_blink(nui_led_state_t st, int times)
{
    if (!s_en || !s_timer) return;

    s_color = st;
    s_left  = (times <= 0) ? -1 : times;

    esp_timer_stop(s_timer);

    s_lit = true;
    apply_color(s_color);

    esp_timer_start_periodic(s_timer, BLINK_MS * 1000);
}

void nui_led_flash(nui_led_state_t st) { nui_led_blink(st, 1); }

void nui_led_stop(void)
{
    if (s_timer) esp_timer_stop(s_timer);
    s_lit = false;
    apply_off();
}
