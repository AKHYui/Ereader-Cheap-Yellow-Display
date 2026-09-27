#pragma once

#include "rd_page.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  lines;
    int  line_w;
    int  line_n;
    bool line_text;

    bool pending;
} rd_walk_t;

void rd_walk_flush(rd_walk_t *w);

bool rd_walk_feed(rd_page_t *out, rd_walk_t *w, uint32_t c);

void rd_walk_finish(rd_page_t *out, const rd_walk_t *w);

#ifdef __cplusplus
}
#endif
