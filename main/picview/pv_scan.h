#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pv_config.h"

#ifdef __cplusplus
extern "C" {
#endif

int pv_scan_run(void);

int pv_scan_count(void);

void pv_scan_path(int idx, char *out, size_t outsz);

bool pv_scan_is_jpeg(int idx);

const char *pv_scan_name(int idx);

#if PV_SCAN_SELFTEST

void pv_scan_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
