#pragma once

#include "sd_card.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RD_LIST_DIR   SD_DIR_NOVELS

#define RD_LIST_DIR_LEN  (sizeof(RD_LIST_DIR) - 1)

void rd_list_run(void);

#ifndef RD_BOOT_SELFTEST
#define RD_BOOT_SELFTEST 0
#endif

#if RD_BOOT_SELFTEST
void rd_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
