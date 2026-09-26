#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"

#define SD_DIR_IMAGES  SD_MOUNT_POINT "/images"
#define SD_DIR_NOVELS  SD_MOUNT_POINT "/novels"

esp_err_t sd_card_mount(void);

void sd_card_ensure_dirs(void);

void      sd_card_unmount(void);
bool      sd_card_is_mounted(void);
uint64_t  sd_card_total_bytes(void);

#ifdef __cplusplus
}
#endif
