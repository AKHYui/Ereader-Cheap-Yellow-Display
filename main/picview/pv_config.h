#pragma once

#include "sd_card.h"

#ifndef PICVIEW_STANDALONE
#define PICVIEW_STANDALONE 1
#endif

#define PV_SCR_W        240
#define PV_SCR_H        320
#define PV_BAR_H         52
#define PV_IMG_H        (PV_SCR_H - PV_BAR_H)

#define PV_BAND_H        64

#define PV_BG         0x0000
#define PV_BAR_BG     0x18E3
#define PV_BTN_BG     0x39E7
#define PV_BTN_PRESS  0x07E0
#define PV_BTN_FG     0xFFFF
#define PV_TXT_FG     0xFFFF
#define PV_BAD        0xF800

#define PV_DIR        SD_DIR_IMAGES
#define PV_MAX_FILES    300

#define PV_NAME_POOL   20480

#define PV_PATH_MAX     300

#define PV_BTN_W         44
#define PV_BTN_H         44
#define PV_BTN_MARGIN     4
#define PV_DEL_W         44
#define PV_AUTOPLAY_MS 0

#define PV_HOME_W        88
#define PV_HOME_H        44

#ifndef PV_RGB565_SWAP
#define PV_RGB565_SWAP    1
#endif

#ifndef PV_COLORTEST
#define PV_COLORTEST      0

#endif

#ifndef PV_CARD_FIRST
#define PV_CARD_FIRST     0
#endif

#ifndef PV_BUILTIN_CARD
#define PV_BUILTIN_CARD   1
#endif

#ifndef PV_TOUCH_LOG
#define PV_TOUCH_LOG      0
#endif
#define PV_TOUCH_LOG_PATH "/sdcard/touch.log"

#ifndef PV_SCAN_SELFTEST
#define PV_SCAN_SELFTEST  0

#endif

#ifndef PV_DEL_LOG
#define PV_DEL_LOG        1
#endif
#define PV_DEL_LOG_PATH   "/sdcard/del.log"

#define PV_TAG          "picview"
#define PV_STATS          1
