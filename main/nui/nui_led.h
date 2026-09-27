#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NUI_LED_WHITE = 0,
    NUI_LED_OK,
    NUI_LED_BUSY,
    NUI_LED_WARN,
    NUI_LED_ERR,
} nui_led_state_t;

void nui_led_init(void);

bool nui_led_enabled(void);

void nui_led_set_enabled(bool on);

void nui_led_blink(nui_led_state_t st, int times);

void nui_led_flash(nui_led_state_t st);

void nui_led_stop(void);

#ifdef __cplusplus
}
#endif
