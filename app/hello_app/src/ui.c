/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui.c - watch UI toolkit (see ui.h).
 *
 * 5x7 font, 7-segment digits, progress rings, pixel icons, cards,
 * badges, headers and page indicator dots - all drawing into the
 * RGB565 framebuffer owned by the renderer.
 */

#include "ui.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * 5x7 font (ASCII 0x20..0x7E)
 * ------------------------------------------------------------------ */

static const uint8_t g_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x32}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x41,0x41,0x7F,0x00,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x08,0x14,0x54,0x54,0x3C}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x00,0x7F,0x10,0x28,0x44}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x08,0x2A,0x1C,0x08}, /* ~ */
};

/* ------------------------------------------------------------------ *
 * pixel icons (16x16, MSB = left)
 * ------------------------------------------------------------------ */

static const uint16_t g_icons[UI_ICON_COUNT][16] = {
    /* UI_ICON_RUN */
    { 0x0000, 0x0000, 0x07C0, 0x0820, 0x0820, 0x0820, 0x0820, 0x1020, 0x2020, 0x4020, 0x8020, 0x4020, 0x2040, 0x1FC0, 0x0000, 0x0000 },
    /* UI_ICON_ROUTE */
    { 0x0000, 0x0000, 0x7BF8, 0x0A08, 0x0A08, 0x0BF8, 0x0820, 0x7820, 0x8420, 0x8440, 0x8480, 0x8500, 0x8600, 0x7800, 0x0000, 0x0000 },
    /* UI_ICON_STATS */
    { 0x0000, 0x0000, 0x0200, 0x0200, 0x0200, 0x0200, 0x1200, 0x1200, 0x1200, 0x1200, 0x1200, 0x1200, 0x1200, 0x7C00, 0x0000, 0x0000 },
    /* UI_ICON_GEAR */
    { 0x0000, 0x0840, 0x0840, 0x3FF0, 0x2790, 0x1020, 0x1020, 0x1020, 0x1020, 0x1020, 0x2790, 0x3FF0, 0x0840, 0x0840, 0x0000, 0x0000 },
    /* UI_ICON_BT */
    { 0x0000, 0x0300, 0x0480, 0x0880, 0x1080, 0x2080, 0x1080, 0x0880, 0x0480, 0x0300, 0x0300, 0x0480, 0x0880, 0x1080, 0x2080, 0x1080 },
    /* UI_ICON_BAT */
    { 0x0000, 0x1FF8, 0x2004, 0x27E4, 0x27E4, 0x27E4, 0x27E4, 0x2004, 0x1FF8, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_HEART */
    { 0x0000, 0x3030, 0x7878, 0xFCFC, 0xFFFC, 0xFFFC, 0x7FF8, 0x3FF0, 0x1FE0, 0x0FC0, 0x0780, 0x0300, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_FLAG */
    { 0x0000, 0x0800, 0x09F8, 0x0A04, 0x0A04, 0x09F8, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0800, 0x0000 },
    /* UI_ICON_SUN */
    { 0x0000, 0x0840, 0x0480, 0x0300, 0x0780, 0x0840, 0x1020, 0x1020, 0x1020, 0x0840, 0x0780, 0x0300, 0x0480, 0x0840, 0x0000, 0x0000 },
    /* UI_ICON_COMPASS */
    { 0x0000, 0x0780, 0x0840, 0x1320, 0x2490, 0x2490, 0x2110, 0x1108, 0x0908, 0x0420, 0x0240, 0x03C0, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_STEPS */
    { 0x0000, 0x0000, 0x0600, 0x1B00, 0x3300, 0x3300, 0x3B80, 0x1F00, 0x0000, 0x0180, 0x06C0, 0x0CB0, 0x0CB0, 0x0FD8, 0x07C0, 0x0000 },
    /* UI_ICON_PLAY */
    { 0x0000, 0x0000, 0x0380, 0x07E0, 0x0FF0, 0x1FF8, 0x1FF8, 0x0FF0, 0x07E0, 0x0380, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_PAUSE */
    { 0x0000, 0x0000, 0x0E70, 0x0E70, 0x0E70, 0x0E70, 0x0E70, 0x0E70, 0x0E70, 0x0E70, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_CHECK */
    { 0x0000, 0x0000, 0x0002, 0x0006, 0x0004, 0x0018, 0x0028, 0x0048, 0x0088, 0x0108, 0x0208, 0x0408, 0x0804, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_INFO */
    { 0x0000, 0x0000, 0x0780, 0x0480, 0x0480, 0x0780, 0x0400, 0x0400, 0x0400, 0x0400, 0x0400, 0x0400, 0x0400, 0x0000, 0x0000, 0x0000 },
    /* UI_ICON_TARGET */
    { 0x0000, 0x07C0, 0x0820, 0x1008, 0x11C8, 0x1248, 0x1248, 0x1248, 0x11C8, 0x1008, 0x0820, 0x07C0, 0x0000, 0x0000, 0x0000, 0x0000 },
};

void ui_icon(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, ui_icon_t ic, pixel_t color)
{
    int py, px;
    if (ic >= UI_ICON_COUNT)
        return;
    for (py = 0; py < 16; py++)
    {
        uint16_t row = g_icons[ic][py];
        for (px = 0; px < 16; px++)
        {
            int bx = x + px, by = y + py;
            if (row & (0x8000 >> px))
            {
                if (bx >= 0 && bx < buf_w && by >= 0 && by < buf_h)
                    buf[by * buf_w + bx] = color;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * text
 * ------------------------------------------------------------------ */

void ui_text_scaled(pixel_t *buf, int buf_w, int buf_h,
                    int x, int y, const char *s, int scale, pixel_t color)
{
    int px, col, cyy;
    int sx, sy;
    char ch;

    if (scale < 1)
        scale = 1;
    while ((ch = *s++) != '\0')
    {
        if (ch == ' ')
        {
            x += 6 * scale;
            continue;
        }
        if (ch < 0x20 || ch > 0x7E)
            ch = '?';
        const uint8_t *glyph = g_font5x7[ch - 0x20];
        for (col = 0; col < 5; col++)
        {
            for (cyy = 0; cyy < 7; cyy++)
            {
                if (!(glyph[col] & (1 << cyy)))
                    continue;
                for (sy = 0; sy < scale; sy++)
                {
                    for (sx = 0; sx < scale; sx++)
                    {
                        px = x + col * scale + sx;
                        int py2 = y + cyy * scale + sy;
                        if (px >= 0 && px < buf_w &&
                            py2 >= 0 && py2 < buf_h)
                            buf[py2 * buf_w + px] = color;
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

void ui_text(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, const char *s, pixel_t color)
{
    int px, py, col;
    char ch;

    while ((ch = *s++) != '\0')
    {
        if (ch == ' ')
        {
            x += 6;
            continue;
        }
        if (ch < 0x20 || ch > 0x7E)
            ch = '?';
        const uint8_t *glyph = g_font5x7[ch - 0x20];
        for (col = 0; col < 5; col++)
        {
            for (py = 0; py < 7; py++)
            {
                if (glyph[col] & (1 << py))
                {
                    px = x + col;
                    if (px >= 0 && px < buf_w && y + py >= 0 && y + py < buf_h)
                        buf[(y + py) * buf_w + px] = color;
                }
            }
        }
        x += 6;
    }
}

/* ------------------------------------------------------------------ *
 * 7-segment digits
 * ------------------------------------------------------------------ */

static void seg_hline(pixel_t *buf, int bw, int bh, int x, int y,
                      int len, int th, pixel_t c)
{
    int i, j;
    for (j = 0; j < th; j++)
        for (i = 0; i < len; i++)
        {
            int px = x + i, py = y + j;
            if (px >= 0 && px < bw && py >= 0 && py < bh)
                buf[py * bw + px] = c;
        }
}

static void seg_vline(pixel_t *buf, int bw, int bh, int x, int y,
                      int len, int th, pixel_t c)
{
    int i, j;
    for (j = 0; j < len; j++)
        for (i = 0; i < th; i++)
        {
            int px = x + i, py = y + j;
            if (px >= 0 && px < bw && py >= 0 && py < bh)
                buf[py * bw + px] = c;
        }
}

void ui_digit(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, char digit, int scale, pixel_t color)
{
    const int s = scale > 0 ? scale : 4;
    const int w = s;          /* segment thickness */
    const int hw = 4 * s;     /* half digit width  */
    const int hh = 5 * s;     /* half digit height */
    bool a, b, c, d, e, f, g;

    if (digit < '0' || digit > '9')
        return;

    switch (digit)
    {
    case '0': a=b=c=d=e=f=true;  g=false; break;
    case '1': a=d=e=f=g=false;   b=c=true; break;
    case '2': a=b=d=e=g=true;    c=f=false; break;
    case '3': a=b=c=d=g=true;    e=f=false; break;
    case '4': b=c=f=g=true;      a=d=e=false; break;
    case '5': a=c=d=f=g=true;    b=e=false; break;
    case '6': a=c=d=e=f=g=true;  b=false; break;
    case '7': a=b=c=true;        d=e=f=g=false; break;
    case '8': a=b=c=d=e=f=g=true; break;
    default:  a=b=c=d=e=f=true;  g=false; break; /* 9 */
    }

    if (a) seg_hline(buf, buf_w, buf_h, x + w, y, hw * 2 - w, w, color);
    if (b) seg_vline(buf, buf_w, buf_h, x + hw * 2 - w + w, y + w, hh - w, w, color);
    if (c) seg_vline(buf, buf_w, buf_h, x + hw * 2 - w + w, y + hh + w, hh - w, w, color);
    if (d) seg_hline(buf, buf_w, buf_h, x + w, y + hh * 2 - w, hw * 2 - w, w, color);
    if (e) seg_vline(buf, buf_w, buf_h, x, y + hh + w, hh - w, w, color);
    if (f) seg_vline(buf, buf_w, buf_h, x, y + w, hh - w, w, color);
    if (g) seg_hline(buf, buf_w, buf_h, x + w, y + hh, hw * 2 - w, w, color);
}

void ui_number(pixel_t *buf, int buf_w, int buf_h,
               int x, int y, const char *num, int scale, pixel_t color)
{
    const int s = scale > 0 ? scale : 4;
    const int char_w = 9 * s + 2;

    while (*num)
    {
        char ch = *num++;
        if (ch == ':')
        {
            int cx = x + 2 * s;
            seg_vline(buf, buf_w, buf_h, cx, y + 2 * s, 2 * s, 2 * s, color);
            seg_vline(buf, buf_w, buf_h, cx, y + 6 * s, 2 * s, 2 * s, color);
            x += 5 * s;
        }
        else if (ch == '.')
        {
            seg_hline(buf, buf_w, buf_h, x + 2 * s, y + 8 * s, 3 * s, 2 * s, color);
            x += 5 * s;
        }
        else
        {
            ui_digit(buf, buf_w, buf_h, x, y, ch, s, color);
            x += char_w;
        }
    }
}

/* ------------------------------------------------------------------ *
 * shapes & containers
 * ------------------------------------------------------------------ */

void ui_fill(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, int w, int h, pixel_t color)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
        {
            int px = x + i, py = y + j;
            if (px >= 0 && px < buf_w && py >= 0 && py < buf_h)
                buf[py * buf_w + px] = color;
        }
}

void ui_hline(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, int len, pixel_t color)
{
    int i;
    for (i = 0; i < len; i++)
    {
        int px = x + i;
        if (px >= 0 && px < buf_w && y >= 0 && y < buf_h)
            buf[y * buf_w + px] = color;
    }
}

void ui_vline(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, int len, pixel_t color)
{
    int j;
    for (j = 0; j < len; j++)
    {
        int py = y + j;
        if (x >= 0 && x < buf_w && py >= 0 && py < buf_h)
            buf[py * buf_w + x] = color;
    }
}

static void rrect_px(pixel_t *buf, int bw, int bh, int x, int y,
                     int w, int h, int r, pixel_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
        {
            int px = x + i, py = y + j;
            int dx, dy;
            if (px < 0 || px >= bw || py < 0 || py >= bh)
                continue;
            /* corner rounding */
            dx = (i < r) ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
            dy = (j < r) ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
            if (dx > 0 && dy > 0 && dx * dx + dy * dy > r * r)
                continue;
            buf[py * bw + px] = c;
        }
}

void ui_rrect(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, int w, int h, int r, pixel_t color)
{
    /* outline: 4 side strips + 4 corner arcs */
    rrect_px(buf, buf_w, buf_h, x, y, w, 1, 0, color);                    /* top */
    rrect_px(buf, buf_w, buf_h, x, y + h - 1, w, 1, 0, color);            /* bottom */
    rrect_px(buf, buf_w, buf_h, x, y, 1, h, 0, color);                    /* left */
    rrect_px(buf, buf_w, buf_h, x + w - 1, y, 1, h, 0, color);            /* right */
    /* corner arcs */
    rrect_px(buf, buf_w, buf_h, x, y, r + 1, r + 1, r, color);
    rrect_px(buf, buf_w, buf_h, x + w - r - 1, y, r + 1, r + 1, r, color);
    rrect_px(buf, buf_w, buf_h, x, y + h - r - 1, r + 1, r + 1, r, color);
    rrect_px(buf, buf_w, buf_h, x + w - r - 1, y + h - r - 1, r + 1, r + 1, r, color);
}

void ui_card(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, int w, int h, int r, pixel_t color)
{
    rrect_px(buf, buf_w, buf_h, x, y, w, h, r, color);
}

/* ------------------------------------------------------------------ *
 * progress ring (plain)
 * ------------------------------------------------------------------ */

void ui_ring(pixel_t *buf, int buf_w, int buf_h,
             int cx, int cy, int r, float frac,
             pixel_t color, pixel_t bg)
{
    int x, y;
    int r2 = r * r;
    int rw2 = (r - 2) * (r - 2);

    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    for (y = -r; y <= r; y++)
    {
        for (x = -r; x <= r; x++)
        {
            int d = x * x + y * y;
            if (d < r2 && d >= rw2)
            {
                int px = cx + x, py = cy + y;
                if (px < 0 || px >= buf_w || py < 0 || py >= buf_h)
                    continue;
                float ang = atan2f((float)y, (float)x) * 0.159154943f; /* /2pi */
                if (ang < 0.0f) ang += 1.0f;
                /* frac from 12 o'clock, clockwise */
                float t = ang - 0.25f;
                if (t < 0.0f) t += 1.0f;
                buf[py * buf_w + px] = (t < frac) ? color : bg;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * sport ring: outer tick ring + smooth arc
 * ------------------------------------------------------------------ */

void ui_ring_sport(pixel_t *buf, int buf_w, int buf_h,
                   int cx, int cy, int r, float frac,
                   pixel_t color, pixel_t bg, int ticks)
{
    int x, y;
    int r2 = r * r;
    int rw2 = (r - 2) * (r - 2);
    int rt2 = (r - 1) * (r - 1);
    int rtw2 = (r - 4) * (r - 4);

    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (ticks < 4) ticks = 12;

    for (y = -r; y <= r; y++)
    {
        for (x = -r; x <= r; x++)
        {
            int d = x * x + y * y;
            int px = cx + x, py = cy + y;
            if (px < 0 || px >= buf_w || py < 0 || py >= buf_h)
                continue;
            if (d < r2 && d >= rw2)
            {
                float ang = atan2f((float)y, (float)x) * 0.159154943f;
                if (ang < 0.0f) ang += 1.0f;
                float t = ang - 0.25f;
                if (t < 0.0f) t += 1.0f;
                /* tick marks every 1/ticks of the full circle */
                float slot = t * ticks;
                int sloti = (int)slot;
                bool tick = (slot - sloti) < 0.12f;
                if (tick)
                    buf[py * buf_w + px] = (t < frac) ? color : bg;
                else
                    buf[py * buf_w + px] = (t < frac) ? color : bg;
            }
            else if (d < rt2 && d >= rtw2)
            {
                buf[py * buf_w + px] = bg;
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * bar
 * ------------------------------------------------------------------ */

void ui_bar(pixel_t *buf, int buf_w, int buf_h,
            int x, int y, int w, int h, float frac,
            pixel_t color, pixel_t bg)
{
    int i, j;
    int filled = (int)(w * (frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac)));

    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
        {
            int px = x + i, py = y + j;
            if (px < 0 || px >= buf_w || py < 0 || py >= buf_h)
                continue;
            buf[py * buf_w + px] = (i < filled) ? color : bg;
        }
}

/* ------------------------------------------------------------------ *
 * composite components
 * ------------------------------------------------------------------ */

void ui_header(pixel_t *buf, int buf_w, int buf_h,
               const char *title, ui_icon_t ic)
{
    ui_fill(buf, buf_w, buf_h, 0, 0, buf_w, 28, UI_CARD);
    ui_hline(buf, buf_w, buf_h, 0, 28, buf_w, UI_CARD_EDGE);
    ui_icon(buf, buf_w, buf_h, 24, 6, ic, UI_ACCENT);
    ui_text(buf, buf_w, buf_h, 48, 10, title, UI_TEXT);
}

void ui_badge(pixel_t *buf, int buf_w, int buf_h,
              int cx, int y, const char *s,
              pixel_t fg, pixel_t bg)
{
    int len = (int)strlen(s);
    int w = len * 6 + 16;
    int x = cx - w / 2;
    ui_card(buf, buf_w, buf_h, x, y, w, 16, 8, bg);
    ui_text(buf, buf_w, buf_h, x + 8, y + 4, s, fg);
}

void ui_dots(pixel_t *buf, int buf_w, int buf_h, page_id_t cur)
{
    int i;
    int n = PAGE_COUNT;
    int gap = 12;
    int x0 = buf_w / 2 - ((n - 1) * gap) / 2;
    int y = buf_h - 10;
    for (i = 0; i < n; i++)
    {
        int cx = x0 + i * gap;
        bool on = (i == (int)cur);
        if (on)
        {
            int j;
            for (j = -2; j <= 2; j++)
                buf[y * buf_w + cx + j] = UI_ACCENT;
        }
        else
        {
            buf[y * buf_w + cx] = UI_TEXT_FAINT;
        }
    }
}
