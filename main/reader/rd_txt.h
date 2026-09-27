#pragma once

#include "rd_page.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool rd_txt_open(const char *path);

void rd_txt_close(void);

bool rd_txt_is_open(void);

const char *rd_txt_title(void);

const char *rd_txt_file(void);

const char *rd_txt_encoding(void);

int  rd_txt_bytes(void);
int  rd_txt_pages(void);
int  rd_txt_page(void);
bool rd_txt_goto(int idx);
bool rd_txt_next(void);
bool rd_txt_prev(void);
int  rd_txt_percent(void);

bool rd_txt_load_page(rd_page_t *out);

void rd_name_utf8(const char *src, char *dst, int dst_len);

#ifdef __cplusplus
}
#endif
