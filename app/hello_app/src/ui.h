/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui.h - watch UI toolkit: page manager, text renderer, 7-segment
 * digits, progress rings, pixel icons, cards, badges and a status bar.
 * All drawing goes into the RGB565 framebuffer owned by the renderer.
 */

#ifndef __HUANGSHAN_UI_H
#define __HUANGSHAN_UI_H

#include <stdint.h>
#include <stdbool.h>

#include "route_renderer.h"   /* pixel_t, render_state_t */

/* ------------------------------------------------------------------ *
 * design tokens (deep-dark sport theme)
 * ------------------------------------------------------------------ */

#define UI_BG             PIXEL_BLACK
#define UI_CARD           0x2124      /* dark slate card            */
#define UI_CARD_EDGE      0x39E7      /* slightly lighter card edge */
#define UI_ACCENT         0x57E0      /* fluorescent green          */
#define UI_ACCENT_ORANGE  0xFD20      /* sport orange               */
#define UI_ACCENT_BLUE    0x2C7F      /* electric blue              */
#define UI_ACCENT_RED     PIXEL_RED
#define UI_TEXT           PIXEL_WHITE
#define UI_TEXT_DIM       PIXEL_GRAY
#define UI_TEXT_FAINT     0x5AEB

/* ------------------------------------------------------------------ *
 * pages
 * ------------------------------------------------------------------ */

typedef enum {
    PAGE_WATCH = 0,   /* watch face: time/date/steps */
    PAGE_RUN,         /* run: data view <-> map view */
    PAGE_ROUTE,       /* map: route preview / live trail / review */
    PAGE_COMPASS,     /* heading dial + calibration */
    PAGE_STATS,       /* totals + history */
    PAGE_SETTINGS,    /* brightness / units / decl / compass cal */
    PAGE_COUNT,
} page_id_t;

typedef struct {
    page_id_t current;
    page_id_t prev;
    uint32_t  page_start_ms;
    bool      dirty;
    bool      animating;   /* page-entrance animation in progress */
    uint32_t  anim_ms;     /* animation start timestamp */
    int       slide_dir;   /* +1 next (slide in from right), -1 prev */
    bool      run_map_view;/* PAGE_RUN shows the map sub-view */
} ui_state_t;

/* ------------------------------------------------------------------ *
 * pixel icons (16x16, indexed into a bitmap table)
 * ------------------------------------------------------------------ */

typedef enum {
    UI_ICON_RUN = 0,    /* running shoe */
    UI_ICON_ROUTE,      /* map + flag   */
    UI_ICON_STATS,      /* bar chart    */
    UI_ICON_GEAR,       /* settings     */
    UI_ICON_BT,         /* bluetooth    */
    UI_ICON_BAT,        /* battery      */
    UI_ICON_HEART,      /* heart        */
    UI_ICON_FLAG,       /* finish flag  */
    UI_ICON_SUN,        /* brightness   */
    UI_ICON_COMPASS,    /* compass      */
    UI_ICON_STEPS,      /* footprint    */
    UI_ICON_PLAY,       /* play         */
    UI_ICON_PAUSE,      /* pause        */
    UI_ICON_CHECK,      /* checkmark    */
    UI_ICON_INFO,       /* info         */
    UI_ICON_TARGET,     /* target / goal*/
    UI_ICON_COUNT
} ui_icon_t;

void ui_icon(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, ui_icon_t ic, pixel_t color);

/* ------------------------------------------------------------------ *
 * text
 * ------------------------------------------------------------------ */

void ui_text(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, const char *s, pixel_t color);

/* same 5x7 font drawn at an integer scale (2 = 10x14 px blocks); use for
 * anything that must stay readable on the 390x450 panel */
void ui_text_scaled(pixel_t *buf, int buf_w, int buf_h,
                    int x, int y, const char *s, int scale, pixel_t color);

/* big 7-segment digits (watch-style), scale = digit height in pixels/8 */
void ui_digit(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, char digit, int scale, pixel_t color);
void ui_number(pixel_t *buf, int buf_w, int buf_h,
               int x, int y, const char *num, int scale, pixel_t color);

/* ------------------------------------------------------------------ *
 * shapes & containers
 * ------------------------------------------------------------------ */

void ui_fill(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, int w, int h, pixel_t color);

/* rounded rectangle outline (1px) */
void ui_rrect(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, int w, int h, int r, pixel_t color);

/* filled rounded rectangle (card) */
void ui_card(pixel_t *buf, int buf_w, int buf_h,
             int x, int y, int w, int h, int r, pixel_t color);

void ui_hline(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, int len, pixel_t color);
void ui_vline(pixel_t *buf, int buf_w, int buf_h,
              int x, int y, int len, pixel_t color);

/* progress ring: frac in [0,1], clockwise from 12 o'clock */
void ui_ring(pixel_t *buf, int buf_w, int buf_h,
             int cx, int cy, int r, float frac,
             pixel_t color, pixel_t bg);

/* dashed progress ring with tick marks (sport style) */
void ui_ring_sport(pixel_t *buf, int buf_w, int buf_h,
                   int cx, int cy, int r, float frac,
                   pixel_t color, pixel_t bg, int ticks);

/* filled rounded bar, used for settings sliders */
void ui_bar(pixel_t *buf, int buf_w, int buf_h,
            int x, int y, int w, int h, float frac,
            pixel_t color, pixel_t bg);

/* ------------------------------------------------------------------ *
 * composite components
 * ------------------------------------------------------------------ */

/* top status strip: [icon] [title] .... [bt] [battery] */
void ui_header(pixel_t *buf, int buf_w, int buf_h,
               const char *title, ui_icon_t ic);

/* rounded pill badge, centered at cx */
void ui_badge(pixel_t *buf, int buf_w, int buf_h,
              int cx, int y, const char *s,
              pixel_t fg, pixel_t bg);

/* page indicator dots at the bottom */
void ui_dots(pixel_t *buf, int buf_w, int buf_h, page_id_t cur);

#endif /* __HUANGSHAN_UI_H */
