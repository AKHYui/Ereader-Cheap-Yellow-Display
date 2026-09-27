#pragma once

#include "rd_epub.h"
#include "rd_page.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool rd_book_path_is_epub(const char *path);

bool rd_book_path_is_book(const char *path);

bool rd_book_open(const char *path, rd_prog_fn prog);

void rd_book_close(void);
bool rd_book_is_open(void);

const char *rd_book_title(void);
const char *rd_book_file(void);
const char *rd_book_kind(void);
const char *rd_book_encoding(void);

int  rd_book_bytes(void);
int  rd_book_pages(void);
int  rd_book_page(void);
bool rd_book_goto(int idx);
bool rd_book_next(void);
bool rd_book_prev(void);
int  rd_book_percent(void);

bool rd_book_load_page(rd_page_t *out);

#ifdef __cplusplus
}
#endif
