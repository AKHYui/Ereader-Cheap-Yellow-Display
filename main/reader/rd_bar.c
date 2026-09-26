#include "rd_bar.h"

#include "nui_ui.h"
#include "rd_font.h"

#include <stddef.h>

int rd_bar_x(int i, int n)
{
    if (n < 1) n = 1;
    if (n > RD_BAR_MAX) n = RD_BAR_MAX;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;

    const int total = n * RD_BTN_W + (n - 1) * RD_BTN_GAP;
    const int start = (PV_SCR_W - total + 1) / 2;
    return start + i * (RD_BTN_W + RD_BTN_GAP);
}

int rd_bar_y(void)
{
    return RD_BAR_Y + (RD_BAR_H - RD_BTN_H) / 2;
}

void rd_bar_row(uint16_t *row, int sy, int n, const char *const *labels, int pressed)
{
    if (!row || !labels) return;
    if (n < 1) return;
    if (n > RD_BAR_MAX) n = RD_BAR_MAX;

    const int y = rd_bar_y();
    if (sy < y || sy >= y + RD_BTN_H) return;

    for (int i = 0; i < n; i++) {
        const uint16_t bg = (pressed == i + 1) ? NUI_PRESS : NUI_PANEL;
        const int      x  = rd_bar_x(i, n);

        nui_frame_row(row, sy, x, y, RD_BTN_W, RD_BTN_H, bg, NUI_EDGE, 2);

        const char *s = labels[i];
        if (!s || !s[0]) continue;

        uint32_t cps[8];
        int m = rd_cp_from_utf8(s, cps, 8);

        m = rd_cp_fit(cps, m, RD_BTN_W - 6);
        const int w = rd_font_width(cps, m);
        rd_font_row(row, sy, x + (RD_BTN_W - w) / 2,
                    y + (RD_BTN_H - RD_CELL_H) / 2, cps, m, NUI_FG, bg);
    }
}

int rd_bar_hit(int x, int y, int n)
{
    if (n < 1) return 0;
    if (n > RD_BAR_MAX) n = RD_BAR_MAX;

    const int by = rd_bar_y();
    if (y < by || y >= by + RD_BTN_H) return 0;

    for (int i = 0; i < n; i++) {
        const int bx = rd_bar_x(i, n);
        if (x >= bx && x < bx + RD_BTN_W) return i + 1;
    }
    return 0;
}
