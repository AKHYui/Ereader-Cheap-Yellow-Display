#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NUI_SLP_CLOCK = 0, NUI_SLP_WHALE = 1, NUI_SLP_STYLE_N = 2 };

void nui_sleep_init(void);

bool nui_sleep_poll(void);

void nui_sleep_keep_awake(void);

void nui_sleep_run_once(void);

void nui_sleep_test_autoexit(int ms);

void nui_sleep_test_timeout(int s);

void nui_sleep_test_autoback(int on);

bool nui_sleep_test_should_back(void);

int  nui_sleep_timeout(void);
int  nui_sleep_style(void);

const char *nui_sleep_timeout_text(void);

const char *nui_sleep_style_text(void);

void nui_sleep_next_timeout(void);
void nui_sleep_next_style(void);

#ifdef __cplusplus
}
#endif
