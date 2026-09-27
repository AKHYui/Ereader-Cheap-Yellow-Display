#include "rd_book.h"

#include "rd_txt.h"

#include <string.h>
#include <strings.h>

static int s_epub;

static bool ext_is(const char *path, const char *ext)
{
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot + 1, ext);
}

bool rd_book_path_is_epub(const char *path) { return ext_is(path, "epub"); }

bool rd_book_path_is_book(const char *path)
{
    return ext_is(path, "txt") || ext_is(path, "epub");
}

bool rd_book_open(const char *path, rd_prog_fn prog)
{
    s_epub = rd_book_path_is_epub(path) ? 1 : 0;
    if (s_epub) return rd_epub_open(path, prog);
    return rd_txt_open(path);
}

void rd_book_close(void)
{
    if (s_epub) rd_epub_close();
    else        rd_txt_close();
    s_epub = 0;
}

bool rd_book_is_open(void) { return s_epub ? rd_epub_is_open() : rd_txt_is_open(); }

const char *rd_book_title(void)    { return s_epub ? rd_epub_title()    : rd_txt_title(); }
const char *rd_book_file(void)     { return s_epub ? rd_epub_file()     : rd_txt_file(); }
const char *rd_book_kind(void)     { return s_epub ? "EPUB" : "TXT"; }
const char *rd_book_encoding(void) { return s_epub ? "EPUB" : rd_txt_encoding(); }

int  rd_book_bytes(void) { return s_epub ? rd_epub_bytes() : rd_txt_bytes(); }
int  rd_book_pages(void) { return s_epub ? rd_epub_pages() : rd_txt_pages(); }
int  rd_book_page(void)  { return s_epub ? rd_epub_page()  : rd_txt_page(); }

bool rd_book_goto(int idx) { return s_epub ? rd_epub_goto(idx) : rd_txt_goto(idx); }
bool rd_book_next(void)    { return s_epub ? rd_epub_next()    : rd_txt_next(); }
bool rd_book_prev(void)    { return s_epub ? rd_epub_prev()    : rd_txt_prev(); }
int  rd_book_percent(void) { return s_epub ? rd_epub_percent() : rd_txt_percent(); }

bool rd_book_load_page(rd_page_t *out)
{
    return s_epub ? rd_epub_load_page(out) : rd_txt_load_page(out);
}
