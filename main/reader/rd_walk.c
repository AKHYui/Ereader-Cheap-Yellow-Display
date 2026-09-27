#include "rd_walk.h"

#include "rd_font.h"

#include <string.h>

_Static_assert(RD_TEXT_W == 240, "RD_TEXT_W 必须与 PV_SCR_W 一致");

void rd_walk_flush(rd_walk_t *w)
{
    w->lines++;
    w->line_w = 0;
    w->line_n = 0;
    w->line_text = false;
    if (w->lines >= RD_LINES_PER_PAGE) w->pending = true;
}

bool rd_walk_feed(rd_page_t *out, rd_walk_t *w, uint32_t c)
{
    if (c == '\r') return false;
    if (c == '\t') c = ' ';
    if (c < 0x20 && c != '\n') return false;

    if (c == '\n') {

        if (!w->line_text) return false;
        rd_walk_flush(w);
        return false;
    }

    const int a = rd_font_adv(c);
    if (w->line_n > 0 && w->line_w + a > RD_TEXT_W) {
        rd_walk_flush(w);
        if (w->pending) return true;
    }

    if (c != ' ' && c != 0x3000u) w->line_text = true;

    if (out && w->lines < RD_LINES_PER_PAGE && w->line_n < RD_MAX_CPP) {
        out->cp[w->lines][w->line_n] = c;
    }
    w->line_n++;
    w->line_w += a;
    if (out && w->lines < RD_LINES_PER_PAGE && w->line_n <= RD_MAX_CPP) {
        out->n[w->lines] = (uint8_t)w->line_n;
    }
    return false;
}

void rd_walk_finish(rd_page_t *out, const rd_walk_t *w)
{
    if (w->line_n > 0 && w->line_text && w->lines < RD_LINES_PER_PAGE) {
        out->lines = w->lines + 1;
    } else {
        out->lines = w->lines;
    }
}
