#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RD_TEXT_W          240
#define RD_LINES_PER_PAGE   14
#define RD_LINE_H           18
#define RD_TEXT_TOP          2
#define RD_MAX_CPP          32

typedef struct {
    uint32_t cp[RD_LINES_PER_PAGE][RD_MAX_CPP];
    uint8_t  n[RD_LINES_PER_PAGE];
    int      lines;
} rd_page_t;

#ifdef __cplusplus
}
#endif
