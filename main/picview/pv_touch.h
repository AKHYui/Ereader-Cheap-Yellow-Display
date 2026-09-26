#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  x, y;

    int  rx, ry;
    int  z;
    bool down;
} pv_touch_evt_t;

void pv_touch_start(void);

bool pv_touch_ready(void);

bool pv_touch_take(pv_touch_evt_t *evt);

bool pv_touch_last(int *x, int *y);

int pv_touch_hit(int (*hit)(int x, int y, void *ctx), void *ctx);

bool pv_touch_is_down(void);

#ifdef __cplusplus
}
#endif
