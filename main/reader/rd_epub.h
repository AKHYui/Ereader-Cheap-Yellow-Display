#pragma once

#include "rd_page.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rd_prog_fn)(int done, int total);

bool rd_epub_open(const char *path, rd_prog_fn prog);

void rd_epub_close(void);

bool rd_epub_mem_ready(void);

bool        rd_epub_is_open(void);
const char *rd_epub_title(void);
const char *rd_epub_file(void);
int         rd_epub_bytes(void);
int         rd_epub_pages(void);
int         rd_epub_page(void);
bool        rd_epub_goto(int idx);
bool        rd_epub_next(void);
bool        rd_epub_prev(void);
int         rd_epub_percent(void);
bool        rd_epub_load_page(rd_page_t *out);

#ifndef EPUB_SELFTEST
#define EPUB_SELFTEST 0
#endif

#ifndef EPUB_MEM_SELFTEST
#define EPUB_MEM_SELFTEST 0
#endif

#if EPUB_SELFTEST

void rd_epub_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
