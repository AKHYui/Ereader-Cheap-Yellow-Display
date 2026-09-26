#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t net_httpd_start(void);

bool net_httpd_running(void);

#define NET_HTTPD_DEFAULT_DIR  "images"

typedef struct {
    char name[48];
    int  percent;
    int  files;
} net_httpd_prog_t;

void net_httpd_prog(net_httpd_prog_t *out);

#ifdef __cplusplus
}
#endif
