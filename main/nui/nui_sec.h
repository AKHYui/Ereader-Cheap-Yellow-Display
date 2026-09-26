#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool nui_sec_enabled(void);

bool nui_sec_unlock(void);

void nui_sec_run(void);

#ifdef __cplusplus
}
#endif
