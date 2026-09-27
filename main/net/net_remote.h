#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RC_NONE = 0,
    RC_NEXT,
    RC_PREV,
    RC_GOTO,
    RC_OPEN,
    RC_BRIGHT,
    RC_BACK,
} rc_cmd_t;

void net_remote_init(void);

void net_remote_run(void);

bool net_remote_take(rc_cmd_t *cmd, int *arg);

void net_remote_sync(const char *file, int page, int pages);

bool net_remote_post(rc_cmd_t cmd, int arg);

bool net_remote_set_open_path(const char *path);

const char *net_remote_open_path(void);

typedef struct {
    char name[96];

    int  page;
    int  pages;
} net_remote_book_t;

void net_remote_state(net_remote_book_t *out);

#ifdef __cplusplus
}
#endif
