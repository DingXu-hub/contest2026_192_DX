/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pages.c - page layer implementation (see pages.h).
 *
 * v3.2 interaction rules (2026-08):
 *  - NO controls in the screen corners; actions are large buttons on the
 *    vertical centre line of a page.
 *  - NO long-press anywhere.  Everything is a plain short tap.
 *  - Page navigation is done ONLY with the physical keys: KEY1 = previous
 *    page, KEY2 = next page (both short presses, cyclic).  Touching the
 *    screen never changes the page.
 *  - Tap semantics per page: big centre buttons do the work; tapping
 *    elsewhere is a no-op (on the map it is a view helper).
 * Six pages: watch / run (data<->map sub-view) / route / compass /
 * stats / settings.  A full-screen calibration overlay can appear on any
 * page while a figure-8 session runs.
 */

#include "pages.h"
#include "kv_store.h"
#include "ai_agent.h"
#include "link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SCREEN_W 390
#define SCREEN_H 450
#define SAFE_X   24        /* horizontal safe margin (rounded corners) */
#define SAFE_Y   28        /* vertical   safe margin                   */

#define CALIB_MS 15000     /* figure-8 calibration duration */

#define RUN_GOAL_M 5000.0f

/* centre-line action button geometry */
#define BTN_H  46
#define BTN_W1 240         /* single-button width  */
#define BTN_W2 150         /* each of two buttons  */

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */

static void clear_screen(render_state_t *rs, pixel_t bg)
{
    int i;
    if (!rs->cbuf)
        return;
    for (i = 0; i < rs->buf_width * rs->buf_height; i++)
        rs->cbuf[i] = bg;
}

static float ease_out(float t)
{
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* fraction 0..1 for the current page-entrance animation */
static float page_anim(app_ctx_t *ctx, uint32_t now_ms, uint32_t dur_ms)
{
    if (!ctx->ui.animating)
        return 1.0f;
    if (ctx->ui.anim_ms == 0)
        ctx->ui.anim_ms = now_ms;
    return ease_out((float)(now_ms - ctx->ui.anim_ms) / (float)dur_ms);
}

/* ---- readability & rounded-corner safe layout ----------------------
 * The panel corners are rounded, so the usable area is a rounded rect:
 * keep wide content between y=R_TOP..(H-R_BOT) and never put anything in
 * the corner quadrants.  All secondary text is drawn at 2x (10x14 px)
 * because the native 5x7 font is too small on a 390 px panel. */
#define CORNER_R 56          /* corner radius of the visible area */
#define TXT2     2           /* secondary text scale */

/* ---- modern typography helpers ------------------------------------ */

/* tracked (letter-spaced) micro label, centred on the screen */
static void label_center(pixel_t *buf, int bw, int y, const char *s,
                         pixel_t c)
{
    ui_label_center(buf, bw, SCREEN_H, y, s, 2, c, 3);
}

/* same, centred on an arbitrary x */
static void label_cx(pixel_t *buf, int bw, int cx, int y, const char *s,
                     pixel_t c)
{
    int n = (int)strlen(s);
    int w = n * 12 + (n > 1 ? (n - 1) * 3 : 0);
    ui_text_tracked(buf, bw, SCREEN_H, cx - w / 2, y, s, 2, c, 3);
}

static void label_at(pixel_t *buf, int bw, int x, int y, const char *s,
                     pixel_t c)
{
    ui_text_tracked(buf, bw, SCREEN_H, x, y, s, 2, c, 3);
}

/* rounded-stroke number centred on the screen */
static void rnum_center(render_state_t *rs, int y, const char *s, int h,
                        pixel_t c)
{
    ui_rnumber(rs->cbuf, rs->buf_width, rs->buf_height,
               SCREEN_W / 2, y, s, h, c);
}

/* rounded-stroke number centred on an arbitrary x */
static void rnum_cx(render_state_t *rs, int cx, int y, const char *s,
                    int h, pixel_t c)
{
    ui_rnumber(rs->cbuf, rs->buf_width, rs->buf_height, cx, y, s, h, c);
}

/* card with hairline border (the modern container) */
static void card(render_state_t *rs, int x, int y, int w, int h, int r,
                 pixel_t fill)
{
    ui_card_top(rs->cbuf, rs->buf_width, rs->buf_height,
                x, y, w, h, r, fill, UI_CARD_EDGE);
}

/* section label with a short accent bar on the left */
static void section_label(render_state_t *rs, int x, int y, const char *s,
                          pixel_t accent)
{
    ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height, x, y + 4, 4, 10,
               accent);
    label_at(rs->cbuf, rs->buf_width, x + 12, y, s, UI_TEXT_DIM);
}

static void px_line(pixel_t *buf, int bw, int bh,
                    int x0, int y0, int x1, int y1, pixel_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;)
    {
        if (x0 >= 0 && x0 < bw && y0 >= 0 && y0 < bh)
            buf[y0 * bw + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* horizontal slide of the whole framebuffer (page transition) */
static void slide_shift(render_state_t *rs, int dx)
{
    int w = rs->buf_width, h = rs->buf_height;
    int y;
    if (dx > 0)
    {
        for (y = 0; y < h; y++)
        {
            memmove(rs->cbuf + y * w + dx, rs->cbuf + y * w,
                    (size_t)(w - dx) * sizeof(pixel_t));
            memset(rs->cbuf + y * w, 0, (size_t)dx * sizeof(pixel_t));
        }
    }
    else if (dx < 0)
    {
        int d = -dx;
        for (y = 0; y < h; y++)
        {
            memmove(rs->cbuf + y * w, rs->cbuf + y * w + d,
                    (size_t)(w - d) * sizeof(pixel_t));
            memset(rs->cbuf + y * w + (w - d), 0, (size_t)d * sizeof(pixel_t));
        }
    }
}

/* apply the entrance slide after a page has been drawn */
static void page_slide_out(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    const uint32_t dur = 280;
    float t;
    int dx;

    if (!ctx->ui.animating || ctx->ui.anim_ms == 0)
        return;
    t = (float)(now_ms - ctx->ui.anim_ms) / (float)dur;
    if (t >= 1.0f)
        return;
    t = ease_out(t);
    dx = (int)((1.0f - t) * rs->buf_width);
    if (ctx->ui.slide_dir > 0)
        slide_shift(rs, dx);      /* new page slides in from the right */
    else
        slide_shift(rs, -dx);     /* new page slides in from the left  */
}

/* modern action button: filled accent capsule (dark label) or, when
 * UI_TEXT is passed, a hairline "ghost" capsule with a light label */
static void draw_btn(render_state_t *rs, int cx, int y, int w,
                     const char *s, pixel_t c)
{
    if (c == UI_TEXT)
        ui_btn_ghost(rs->cbuf, rs->buf_width, rs->buf_height,
                     cx, y, w, BTN_H, s, UI_TEXT);
    else
        ui_btn(rs->cbuf, rs->buf_width, rs->buf_height,
               cx, y, w, BTN_H, s, c, UI_BG);
}

/* is a tap inside a centre-line button drawn at (cx, y, w)? */
static bool btn_hit(int x, int y, int cx, int by, int w)
{
    return x >= cx - w / 2 && x < cx + w / 2 &&
           y >= by && y < by + BTN_H;
}

/* ------------------------------------------------------------------ *
 * context
 * ------------------------------------------------------------------ */

void app_ctx_init(app_ctx_t *ctx, render_state_t *r, sensor_manager_t *s,
                  power_manager_t *p, gpx_data_t *g)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->renderer = r;
    ctx->sensors  = s;
    ctx->pm       = p;
    ctx->gpx      = g;
    ctx->ui.current = PAGE_WATCH;
    ctx->ui.dirty   = true;
    ctx->brightness_mode = 0;   /* auto */
    run_init(&ctx->run);

    /* kv_get_int() needs a real int32_t* destination; use_miles is bool
     * and brightness_mode is int, so bounce through a temp. */
    {
        int32_t v32;

        if (kv_get_int("unit.miles", &v32, 0) == 0)
            ctx->use_miles = (v32 != 0);
        if (kv_get_int("settings.brightness", &v32, 0) == 0)
            ctx->brightness_mode = (int)v32;
        if (kv_get_int("steps.today", &v32, 0) == 0)
            ctx->today_steps = v32;
        if (kv_get_int("mag.decl", &v32, 0) == 0)
            ctx->decl_deg = (int16_t)v32;
        else
            ctx->decl_deg = 0;
    }
}

void page_set(app_ctx_t *ctx, page_id_t page)
{
    if (page >= PAGE_COUNT)
        page = PAGE_WATCH;
    ctx->ui.prev = ctx->ui.current;
    /* slide direction: +1 when advancing, -1 when going back */
    ctx->ui.slide_dir =
        (page == (page_id_t)((ctx->ui.prev + 1) % PAGE_COUNT)) ? 1 : -1;
    ctx->ui.current = page;
    ctx->ui.run_map_view = false;
    ctx->ui.dirty = true;
    ctx->ui.animating = true;
    ctx->ui.anim_ms = 0;
}

void page_next(app_ctx_t *ctx)
{
    page_set(ctx, (page_id_t)((ctx->ui.current + 1) % PAGE_COUNT));
}

void page_prev(app_ctx_t *ctx)
{
    page_set(ctx, (page_id_t)((ctx->ui.current + PAGE_COUNT - 1) % PAGE_COUNT));
}

/* start a fresh run and reset the live-track auto-fit view; the stored
 * bearing is TRUE north when a declination is configured */
static void start_run(app_ctx_t *ctx)
{
    run_start(&ctx->run);
    if (ctx->sensors)
    {
        float h = sensor_get_heading(ctx->sensors) + ctx->decl_deg;
        ctx->run.heading_deg = h;
        while (ctx->run.heading_deg >= 360.0f)
            ctx->run.heading_deg -= 360.0f;
        while (ctx->run.heading_deg < 0.0f)
            ctx->run.heading_deg += 360.0f;
    }
    ctx->run_saved = false;
    renderer_reset_live_fit(ctx->renderer);
}

/* stop + persist the current run (the STOP button action) */
static void stop_run(app_ctx_t *ctx)
{
    if (ctx->run.state == RUN_RUNNING || ctx->run.state == RUN_PAUSED)
    {
        run_stop(&ctx->run);
        kv_history_push(ctx->run.distance_m, ctx->run.running_ms / 1000,
                        ctx->run.ascent_m);
        ctx->run_saved = true;
        ctx->ui.dirty = true;
    }
}

/* ------------------------------------------------------------------ *
 * page: watch face (time + date + step ring - nothing else)
 * ------------------------------------------------------------------ */

static void page_watch_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    char buf[64];
    time_t t = time(NULL);
    static time_t demo_off;
    struct tm *tm;
    static const char *wdays[] = { "SUN", "MON", "TUE", "WED",
                                   "THU", "FRI", "SAT" };
    static const char *mons[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    const pixel_t acc = ui_accent_for(PAGE_WATCH);
    float anim = page_anim(ctx, now_ms, 700);
    float frac;
    const int goal = 8000;

    /* The board has no RTC, so a cold boot without the PC gateway reads
     * 1970.  The gateway syncs the real time over the link (link.c
     * SET_TIME); when that has happened show the true date/time, and only
     * fall back to a fixed demo date while the clock is still unset. */
    if (t < 1577836800)                 /* before 2020-01-01 => unset */
    {
        if (demo_off == 0)
            demo_off = 1782907200 - t;  /* 2026-07-01 12:00 UTC */
        t += demo_off;
    }
    tm = localtime(&t);

    clear_screen(rs, UI_BG);

    if (!tm)
        return;

    /* micro date line (tracked, dim) */
    snprintf(buf, sizeof(buf), "%s %s %02d",
             wdays[tm->tm_wday], mons[tm->tm_mon], tm->tm_mday);
    label_center(rs->cbuf, rs->buf_width, 30, buf, UI_TEXT_DIM);

    /* step-goal progress as a gradient arc around the clock */
    frac = (float)ctx->today_steps / (float)goal;
    if (frac > 1.0f) frac = 1.0f;
    frac *= anim;
    ui_arc(rs->cbuf, rs->buf_width, rs->buf_height,
           SCREEN_W / 2, 152, 100, 12, frac,
           acc, UI_ACC_LIME, UI_TRACK);

    /* hero time inside the arc */
    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    rnum_center(rs, 124, buf, 54, UI_TEXT);

    snprintf(buf, sizeof(buf), "%d%% OF GOAL", (int)(frac * 100.0f + 0.5f));
    label_center(rs->cbuf, rs->buf_width, 192, buf, UI_TEXT_FAINT);

    /* link indicator, kept on the centre line inside the arc (the four
     * rounded corners stay empty): cyan = PC gateway connected (clock
     * synced / internet reachable), faint = offline */
    {
        bool up = link_gateway_present();
        label_center(rs->cbuf, rs->buf_width, 214,
                     up ? "NET OK" : "NET --",
                     up ? UI_ACC_CYAN : UI_TEXT_FAINT);
    }

    /* today's activity: one card with three metric columns */
    {
        const int cx0 = SAFE_X;
        const int cw = SCREEN_W - 2 * SAFE_X;
        const int cy0 = 276, ch = 96;
        const int colw = cw / 3;
        const int vy = cy0 + 22, ly = cy0 + 62;
        int i;

        card(rs, cx0, cy0, cw, ch, 22, UI_CARD);
        for (i = 1; i < 3; i++)
            ui_vline(rs->cbuf, rs->buf_width, rs->buf_height,
                     cx0 + i * colw, cy0 + 18, ch - 36, UI_CARD_EDGE);

        snprintf(buf, sizeof(buf), "%ld", (long)ctx->today_steps);
        rnum_cx(rs, cx0 + colw / 2, vy, buf, 30, UI_TEXT);
        label_cx(rs->cbuf, rs->buf_width, cx0 + colw / 2, ly,
                 "STEPS", UI_TEXT_DIM);

        snprintf(buf, sizeof(buf), "%.2f",
                 (float)ctx->today_steps * 0.75f / 1000.0f);
        rnum_cx(rs, cx0 + colw + colw / 2, vy, buf, 30, acc);
        label_cx(rs->cbuf, rs->buf_width, cx0 + colw + colw / 2, ly,
                 "KM", UI_TEXT_DIM);

        snprintf(buf, sizeof(buf), "%d",
                 (int)((float)ctx->today_steps * 0.04f));
        rnum_cx(rs, cx0 + 2 * colw + colw / 2, vy, buf, 30,
                UI_ACC_ORANGE);
        label_cx(rs->cbuf, rs->buf_width, cx0 + 2 * colw + colw / 2, ly,
                 "KCAL", UI_TEXT_DIM);
    }

    ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_WATCH);
}

/* ------------------------------------------------------------------ *
 * page: run (data view) - centre buttons START / PAUSE / STOP
 * ------------------------------------------------------------------ */

static const char *run_state_str(run_state_t st)
{
    switch (st)
    {
    case RUN_RUNNING:  return "RUNNING";
    case RUN_PAUSED:   return "PAUSED";
    case RUN_FINISHED: return "DONE";
    default:           return "READY";
    }
}

static pixel_t run_state_color(run_state_t st)
{
    switch (st)
    {
    case RUN_RUNNING:  return UI_ACCENT;
    case RUN_PAUSED:   return UI_ACCENT_ORANGE;
    case RUN_FINISHED: return UI_ACCENT_RED;
    default:           return UI_TEXT_DIM;
    }
}

#define RUN_BTN_Y 344

static void page_run_data_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    run_engine_t *re = &ctx->run;
    char buf[64];
    float frac;
    pixel_t col = run_state_color(re->state);

    (void)now_ms;

    clear_screen(rs, UI_BG);

    /* state pill */
    ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                   SCREEN_W / 2, 20, run_state_str(re->state),
                   col, UI_CARD, col);

    /* hero: elapsed time */
    run_format_elapsed(re, buf, sizeof(buf));
    rnum_center(rs, 58, buf, 68, col);

    /* hero cards: distance (with goal bar) and pace */
    {
        const int cw = (SCREEN_W - 2 * SAFE_X - 12) / 2;
        const int c1x = SAFE_X;
        const int c2x = SAFE_X + cw + 12;
        const int cy = 140, ch = 116;
        char pace[24];

        card(rs, c1x, cy, cw, ch, 20, UI_CARD);
        card(rs, c2x, cy, cw, ch, 20, UI_CARD);

        run_format_distance(re, buf, sizeof(buf), ctx->use_miles);
        rnum_cx(rs, c1x + cw / 2, cy + 20, buf, 40, UI_TEXT);

        frac = re->distance_m / RUN_GOAL_M;
        if (frac > 1.0f) frac = 1.0f;
        if (frac < 0.0f) frac = 0.0f;
        ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height,
                   c1x + 20, cy + 72, cw - 40, 6, UI_TRACK);
        ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height,
                   c1x + 20, cy + 72,
                   (int)((cw - 40) * frac) > 6 ? (int)((cw - 40) * frac) : 6,
                   6, col);
        if (ctx->use_miles)
            snprintf(buf, sizeof(buf), "%.1f MI GOAL", RUN_GOAL_M / 1609.344f);
        else
            snprintf(buf, sizeof(buf), "%.1f KM GOAL", RUN_GOAL_M / 1000.0f);
        label_cx(rs->cbuf, rs->buf_width, c1x + cw / 2, cy + 88, buf,
                 UI_TEXT_DIM);

        run_format_pace(re, pace, sizeof(pace), ctx->use_miles);
        strncpy(buf, pace, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        rnum_cx(rs, c2x + cw / 2, cy + 20, buf, 40, UI_TEXT);
        label_cx(rs->cbuf, rs->buf_width, c2x + cw / 2, cy + 88,
                 ctx->use_miles ? "PACE /MI" : "PACE /KM", UI_TEXT_DIM);
    }

    /* secondary metrics row (no card: keeps the layout airy) */
    {
        const int colw = (SCREEN_W - 2 * SAFE_X) / 3;
        const int vy = 272, ly = 302;
        int i;

        snprintf(buf, sizeof(buf), "%d", (int)(re->cadence_spm + 0.5f));
        rnum_cx(rs, SAFE_X + colw / 2, vy, buf, 24, UI_TEXT_DIM);
        label_cx(rs->cbuf, rs->buf_width, SAFE_X + colw / 2, ly,
                 "CAD", UI_TEXT_FAINT);

        snprintf(buf, sizeof(buf), "%lu", (unsigned long)re->steps);
        rnum_cx(rs, SAFE_X + colw + colw / 2, vy, buf, 24, UI_TEXT_DIM);
        label_cx(rs->cbuf, rs->buf_width, SAFE_X + colw + colw / 2, ly,
                 "STEPS", UI_TEXT_FAINT);

        for (i = 0; i < 2; i++)
            ui_vline(rs->cbuf, rs->buf_width, rs->buf_height,
                     SAFE_X + (i + 1) * colw, vy - 2, 40, UI_CARD_EDGE);

        snprintf(buf, sizeof(buf), "%.0f", re->ascent_m);
        rnum_cx(rs, SAFE_X + 2 * colw + colw / 2, vy, buf, 24, UI_TEXT_DIM);
        label_cx(rs->cbuf, rs->buf_width, SAFE_X + 2 * colw + colw / 2, ly,
                 "ASC M", UI_TEXT_FAINT);
    }

    /* centre action buttons */
    switch (re->state)
    {
    case RUN_RUNNING:
        draw_btn(rs, 112, RUN_BTN_Y, BTN_W2, "PAUSE",
                 UI_ACC_ORANGE);
        draw_btn(rs, 278, RUN_BTN_Y, BTN_W2, "STOP", UI_ACC_RED);
        break;
    case RUN_PAUSED:
        draw_btn(rs, 112, RUN_BTN_Y, BTN_W2, "RESUME", UI_ACC_LIME);
        draw_btn(rs, 278, RUN_BTN_Y, BTN_W2, "STOP", UI_ACC_RED);
        break;
    case RUN_FINISHED:
        draw_btn(rs, SCREEN_W / 2, RUN_BTN_Y, BTN_W1, "RUN AGAIN",
                 UI_ACC_LIME);
        break;
    default:
        draw_btn(rs, SCREEN_W / 2, RUN_BTN_Y, BTN_W1, "START",
                 UI_ACC_LIME);
        break;
    }

    ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_RUN);
}

/* ------------------------------------------------------------------ *
 * map view (standalone route page AND run-page map sub-view)
 * ------------------------------------------------------------------ */

static void page_run_map_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    char buf[64];
    bool live = (ctx->run.state == RUN_RUNNING ||
                 ctx->run.state == RUN_PAUSED);
    bool trail = live ||
                 (ctx->run.state == RUN_FINISHED &&
                  ctx->run.track_count >= 2);

    rs->show_gpx = !trail;
    renderer_set_live_track(rs, trail);
    if (trail)
        rs->needs_redraw = true;
    renderer_update(rs, now_ms);

    if (trail)
    {
        const run_track_point_t *tp;
        int n = run_track_read(&ctx->run, &tp);
        if (n >= 1)
            renderer_draw_run_track(rs, tp, n, ctx->run.heading_deg,
                                    now_ms);
    }


    /* status: floating pill over the map + ghost BACK capsule */
    if (trail)
    {
        run_format_distance(&ctx->run, buf, sizeof(buf), ctx->use_miles);
        {
            char tmp[32], tmp2[24];
            strncpy(tmp2, buf, sizeof(tmp2) - 1);
            tmp2[sizeof(tmp2) - 1] = '\0';
            run_format_elapsed(&ctx->run, tmp, sizeof(tmp));
            snprintf(buf, sizeof(buf), "%s %s  %s",
                     tmp2, ctx->use_miles ? "mi" : "km", tmp);
        }
        ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                       SCREEN_W / 2, 22, buf, UI_TEXT, UI_CARD,
                       ui_accent_for(PAGE_ROUTE));
    }
    else if (ctx->gpx && ctx->gpx->loaded)
    {
        snprintf(buf, sizeof(buf), "ROUTE %.1f%s",
                 ctx->gpx->total_distance_m / 1000.0,
                 ctx->use_miles ? "mi" : "km");
        ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                       SCREEN_W / 2, 22, buf, UI_TEXT, UI_CARD,
                       ui_accent_for(PAGE_ROUTE));
    }
    else
    {
        ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                       SCREEN_W / 2, 22, "NO ROUTE", UI_TEXT_DIM,
                       UI_CARD, 0);
    }

    label_center(rs->cbuf, rs->buf_width, 316, "TAP TO FIT", UI_TEXT_FAINT);
    draw_btn(rs, SCREEN_W / 2, 344, BTN_W1, "BACK", UI_TEXT);
}

/* ------------------------------------------------------------------ *
 * page: compass (rotating dial, centre CALIBRATE button)
 * ------------------------------------------------------------------ */

/* Adaptive smoother with directional confirmation (see git history). */
static float compass_smooth(app_ctx_t *ctx, float target)
{
    static float disp;
    static bool  init;
    static bool  in_band;
    static int   s_dir;
    static int   s_cnt;
    float diff, rate, k;

    if (!init)
    {
        disp = target;
        init = true;
    }
    diff = target - disp;
    while (diff >  180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;

    rate = ctx->sensors ? fabsf(ctx->sensors->gyro_yaw_rate) : 0.0f;

    if (fabsf(diff) > 25.0f || rate > 12.0f)
    {
        in_band = false;
        s_cnt = 0;
        s_dir = 0;
        disp += diff * 0.5f;
        if (disp >= 360.0f) disp -= 360.0f;
        else if (disp < 0.0f) disp += 360.0f;
        return disp;
    }

    if (in_band)
    {
        if (fabsf(diff) < 1.5f)
            return disp;
        in_band = false;
    }
    else if (fabsf(diff) < 0.8f)
    {
        in_band = true;
        return disp;
    }

    {
        int sd = (diff > 1.0f) - (diff < -1.0f);
        if (sd == 0)
        {
            s_cnt = 0;
            s_dir = 0;
        }
        else if (sd != s_dir)
        {
            s_dir = sd;
            s_cnt = 1;
        }
        else if (s_cnt < 8)
        {
            s_cnt++;
        }
        if (s_cnt < 8)
            return disp;
    }

    k = 0.06f;
    if (fabsf(diff) > 8.0f || rate > 4.0f)
        k = 0.22f;
    disp += diff * k;
    if (disp >= 360.0f) disp -= 360.0f;
    else if (disp < 0.0f) disp += 360.0f;
    return disp;
}

/* 16-wind rose name for the dial readout */
static const char *compass_rose(int deg)
{
    static const char *const names[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    return names[((deg + 11) / 22) % 16];
}

#define CMP_BTN_Y 344

/* keep the dial inside the rounded-corner safe area: with cy=206/R=112 the
 * ring spans y 94..318, the button 344..390 and the top text 20..70 */
static void page_compass_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    sensor_manager_t *sen = ctx->sensors;
    static const char *const card[4] = { "N", "E", "S", "W" };
    char buf[32];
    const int cx = SCREEN_W / 2, cy = 206, R = 112;
    const pixel_t acc = ui_accent_for(PAGE_COMPASS);
    float hdg;
    bool mag_ok;
    int a, deg;

    (void)now_ms;

    clear_screen(rs, UI_BG);

    /* magnetic source state (modern pill) */
    mag_ok = (sen && sen->mag.present && sen->mag.healthy &&
              !sen->mag_dropped);
    if (sen && !sen->mag.present)
        ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                       SCREEN_W / 2, 20, "NO MAG", UI_TEXT_DIM,
                       UI_CARD, 0);
    else if (mag_ok)
        ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                       SCREEN_W / 2, 20, "MAGNETIC", acc, UI_CARD, acc);
    else
        ui_status_pill(rs->cbuf, rs->buf_width, rs->buf_height,
                       SCREEN_W / 2, 20, "GYRO ONLY", UI_TEXT_FAINT,
                       UI_CARD, UI_ACC_ORANGE);

    /* tilt readout (tracked micro line) */
    if (sen && sen->imu.present)
    {
        snprintf(buf, sizeof(buf), "PITCH %+03d  ROLL %+03d",
                 (int)sen->imu.pitch, (int)sen->imu.roll);
        label_center(rs->cbuf, rs->buf_width, 54, buf, UI_TEXT_FAINT);
    }

    hdg = compass_smooth(ctx,
                         (sen ? sen->heading_deg : 0.0f) +
                         (float)ctx->decl_deg);
    while (hdg >= 360.0f) hdg -= 360.0f;
    while (hdg < 0.0f) hdg += 360.0f;
    deg = (int)(hdg + 0.5f) % 360;

    /* bezel track */
    ui_arc(rs->cbuf, rs->buf_width, rs->buf_height,
           cx, cy, R, 2, 0.0f, UI_TRACK, UI_TRACK, UI_TRACK);

    /* rotating tick ring: 5 deg minor, 10 deg medium, 30 deg major */
    for (a = 0; a < 360; a += 5)
    {
        float rel = ((float)a - hdg) * (float)M_PI / 180.0f;
        float sn = sinf(rel), cs = cosf(rel);
        int  len, x1, y1, x2, y2;
        pixel_t col;

        if (a % 90 == 0)      { len = 18; col = UI_TEXT; }
        else if (a % 30 == 0) { len = 15; col = UI_TEXT_DIM; }
        else if (a % 10 == 0) { len = 10; col = UI_TEXT_DIM; }
        else                  { len = 5;  col = UI_TEXT_FAINT; }

        x1 = cx + (int)((R - 6) * sn);
        y1 = cy - (int)((R - 6) * cs);
        x2 = cx + (int)((R - 6 - len) * sn);
        y2 = cy - (int)((R - 6 - len) * cs);
        px_line(rs->cbuf, rs->buf_width, rs->buf_height,
                x1, y1, x2, y2, col);
    }

    /* cardinal letters (N in the accent colour) */
    for (a = 0; a < 360; a += 90)
    {
        float rel = ((float)a - hdg) * (float)M_PI / 180.0f;
        float sn = sinf(rel), cs = cosf(rel);
        int lx = cx + (int)((R - 46) * sn) - 3;
        int ly = cy - (int)((R - 46) * cs) - 4;
        ui_text(rs->cbuf, rs->buf_width, rs->buf_height,
                lx, ly, card[a / 90],
                (a == 0) ? acc : UI_TEXT);
    }

    /* inner hub ring */
    ui_arc(rs->cbuf, rs->buf_width, rs->buf_height,
           cx, cy, 74, 2, 0.0f, UI_CARD_EDGE, UI_CARD_EDGE,
           UI_CARD_EDGE);

    /* fixed lubber mark at 12 o'clock (accent, rounded) */
    {
        int y;
        for (y = 0; y < 15; y++)
        {
            int hw = 8 - (8 * y) / 14;
            int yy = cy - R - 24 + y;
            int x;
            for (x = cx - hw; x <= cx + hw; x++)
                if (x >= 0 && x < SCREEN_W && yy >= 0 && yy < SCREEN_H)
                    rs->cbuf[yy * SCREEN_W + x] = acc;
        }
        ui_dot(rs->cbuf, rs->buf_width, rs->buf_height,
               cx, cy - R, 4, acc);
    }

    /* big bearing readout + wind rose name */
    snprintf(buf, sizeof(buf), "%03d", deg);
    rnum_cx(rs, cx, cy - 34, buf, 54, UI_TEXT);
    label_cx(rs->cbuf, rs->buf_width, cx, cy + 34, compass_rose(deg), acc);

    draw_btn(rs, SCREEN_W / 2, CMP_BTN_Y, BTN_W1, "CALIBRATE", acc);

    ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_COMPASS);
}

/* ------------------------------------------------------------------ *
 * page: stats
 * ------------------------------------------------------------------ */

static void page_stats_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    char buf[64];
    int i, n, runs = 0;
    float total_d = 0.0f;
    uint32_t total_s = 0;
    const pixel_t acc = ui_accent_for(PAGE_STATS);
    const int cw = (SCREEN_W - 2 * SAFE_X - 2 * 12) / 3;
    const int c1x = SAFE_X;
    const int c2x = SAFE_X + cw + 12;
    const int c3x = SAFE_X + 2 * (cw + 12);
    const int cy = 56, ch = 112;

    (void)now_ms;
    clear_screen(rs, UI_BG);

    label_center(rs->cbuf, rs->buf_width, 28, "ACTIVITY", UI_TEXT_DIM);

    n = kv_history_count();
    for (i = 0; i < n; i++)
    {
        float d; uint32_t sec; float asc;
        if (kv_history_get(i, &d, &sec, &asc) < 0)
            continue;
        runs++;
        total_d += d;
        total_s += sec;
    }

    /* three summary cards */
    card(rs, c1x, cy, cw, ch, 18, UI_CARD);
    card(rs, c2x, cy, cw, ch, 18, UI_CARD);
    card(rs, c3x, cy, cw, ch, 18, UI_CARD);

    snprintf(buf, sizeof(buf), "%d", runs);
    rnum_cx(rs, c1x + cw / 2, cy + 26, buf, 32, UI_TEXT);
    label_cx(rs->cbuf, rs->buf_width, c1x + cw / 2, cy + 76, "RUNS",
             UI_TEXT_DIM);

    if (ctx->use_miles)
        snprintf(buf, sizeof(buf), "%.1f", total_d / 1609.344f);
    else
        snprintf(buf, sizeof(buf), "%.1f", total_d / 1000.0f);
    rnum_cx(rs, c2x + cw / 2, cy + 26, buf, 32, acc);
    label_cx(rs->cbuf, rs->buf_width, c2x + cw / 2, cy + 76,
             ctx->use_miles ? "MI" : "KM", UI_TEXT_DIM);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(total_s / 60));
    rnum_cx(rs, c3x + cw / 2, cy + 26, buf, 32, UI_TEXT);
    label_cx(rs->cbuf, rs->buf_width, c3x + cw / 2, cy + 76, "MIN",
             UI_TEXT_DIM);

    /* recent runs list */
    section_label(rs, SAFE_X, 194, "RECENT RUNS", acc);

    if (n == 0)
    {
        label_center(rs->cbuf, rs->buf_width, 268, "NO RUNS YET",
                     UI_TEXT_FAINT);
        ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_STATS);
        return;
    }

    {
        int y = 224;
        int shown = 0;
        for (i = 0; i < n && shown < 5 && y + 30 < 390; i++, shown++)
        {
            float d; uint32_t sec; float asc;
            char tbuf[24];

            if (kv_history_get(i, &d, &sec, &asc) < 0)
                continue;

            snprintf(buf, sizeof(buf), "#%02d", i + 1);
            label_at(rs->cbuf, rs->buf_width, SAFE_X, y + 4, buf,
                     UI_TEXT_FAINT);

            snprintf(buf, sizeof(buf), "%.2f",
                     ctx->use_miles ? d / 1609.344f : d / 1000.0f);
            ui_rnumber(rs->cbuf, rs->buf_width, rs->buf_height,
                       132, y, buf, 22, UI_TEXT);

            snprintf(tbuf, sizeof(tbuf), "%s",
                     ctx->use_miles ? "mi" : "km");
            label_at(rs->cbuf, rs->buf_width, 224, y + 4, tbuf,
                     UI_TEXT_FAINT);

            snprintf(buf, sizeof(buf), "%02lu:%02lu",
                     (unsigned long)(sec / 60), (unsigned long)(sec % 60));
            {
                int bw2 = (int)strlen(buf) * 12 + (int)strlen(buf) * 3;
                label_at(rs->cbuf, rs->buf_width, 366 - bw2, y + 4, buf,
                         UI_TEXT_DIM);
            }

            y += 32;
            if (shown < 4)
                ui_hline(rs->cbuf, rs->buf_width, rs->buf_height,
                         SAFE_X, y - 6, SCREEN_W - 2 * SAFE_X, UI_CARD_EDGE);
        }
    }

    ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_STATS);
}

/* ------------------------------------------------------------------ *
 * page: settings (tap a row)
 * ------------------------------------------------------------------ */

static void page_settings_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    char buf[64];
    const pixel_t acc = ui_accent_for(PAGE_SETTINGS);
    const int x0 = SAFE_X;
    const int cw = SCREEN_W - 2 * SAFE_X;
    const int row_h = 56;
    const int y0 = 44;
    const int tx = x0 + 20;          /* label x inside the card */
    const int rx = x0 + cw - 20;     /* right edge for values */
    int i;

    (void)now_ms;

    clear_screen(rs, UI_BG);
    label_center(rs->cbuf, rs->buf_width, 28, "SETTINGS", UI_TEXT_DIM);

    /* four tappable rows, each a card (hit bands stay y0 + i*row_h) */
    for (i = 0; i < 4; i++)
        card(rs, x0, y0 + i * row_h, cw, 48, 16, UI_CARD);

    /* brightness */
    label_at(rs->cbuf, rs->buf_width, tx, y0 + 16, "BRIGHTNESS",
             UI_TEXT);
    if (ctx->brightness_mode == 0)
        snprintf(buf, sizeof(buf), "AUTO");
    else
        snprintf(buf, sizeof(buf), "%d%%", ctx->brightness_mode);
    label_at(rs->cbuf, rs->buf_width,
             rx - ((int)strlen(buf) * 12 + ((int)strlen(buf) - 1) * 3),
             y0 + 16, buf, acc);

    /* units */
    label_at(rs->cbuf, rs->buf_width, tx, y0 + row_h + 16, "UNITS",
             UI_TEXT);
    label_at(rs->cbuf, rs->buf_width, rx - 27, y0 + row_h + 16,
             ctx->use_miles ? "MI" : "KM", UI_ACC_BLUE);

    /* declination */
    label_at(rs->cbuf, rs->buf_width, tx, y0 + 2 * row_h + 16,
             "DECLINATION", UI_TEXT);
    snprintf(buf, sizeof(buf), "%+d", (int)ctx->decl_deg);
    label_at(rs->cbuf, rs->buf_width,
             rx - ((int)strlen(buf) * 12 + ((int)strlen(buf) - 1) * 3),
             y0 + 2 * row_h + 16, buf,
             ctx->decl_deg == 0 ? UI_TEXT_DIM : UI_ACC_ORANGE);

    /* compass calibration */
    label_at(rs->cbuf, rs->buf_width, tx, y0 + 3 * row_h + 16,
             "COMPASS CAL", UI_TEXT);
    label_at(rs->cbuf, rs->buf_width, rx - 42, y0 + 3 * row_h + 16,
             "TAP", UI_ACC_ORANGE);

    /* about card */
    {
        const int ay = 276;
        ui_card_top(rs->cbuf, rs->buf_width, rs->buf_height,
                    x0, ay, cw, 72, 18, UI_CARD, UI_CARD_EDGE);
        ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height,
                   x0 + 16, ay + 16, 4, 40, acc);
        label_at(rs->cbuf, rs->buf_width, x0 + 32, ay + 14,
                 "HUANGSHAN WATCH", UI_TEXT);
        label_at(rs->cbuf, rs->buf_width, x0 + 32, ay + 40,
                 "UI V4  OPENVELA", UI_TEXT_FAINT);
    }

    ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_SETTINGS);
}

/* ------------------------------------------------------------------ *
 * magnetometer calibration overlay (any page while calib runs)
 * ------------------------------------------------------------------ */

static void draw_calib_overlay(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    mag_mmc5603_t *mag;
    char buf[32];
    uint32_t left_ms;
    float frac, cov;
    int y;

    if (!ctx->sensors)
        return;
    mag = &ctx->sensors->mag;
    if (!mag->calib_running)
        return;

    clear_screen(rs, UI_BG);

    label_center(rs->cbuf, rs->buf_width, 56, "CALIBRATING",
                 UI_ACC_ORANGE);
    label_center(rs->cbuf, rs->buf_width, 82,
                 "FIGURE-8  TUMBLE ALL AXES", UI_TEXT_FAINT);

    left_ms = (mag->calib_end_ms > now_ms) ? mag->calib_end_ms - now_ms : 0;
    if (ctx->calib_dur_ms)
    {
        frac = 1.0f - (float)left_ms / (float)ctx->calib_dur_ms;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
    }
    else
        frac = 0.0f;

    ui_arc(rs->cbuf, rs->buf_width, rs->buf_height,
           SCREEN_W / 2, 198, 64, 12, frac,
           UI_ACC_ORANGE, UI_ACC_LIME, UI_TRACK);

    snprintf(buf, sizeof(buf), "%d", (int)((left_ms + 999) / 1000));
    rnum_center(rs, 172, buf, 52, UI_TEXT);
    label_center(rs->cbuf, rs->buf_width, 272, "SECONDS LEFT",
                 UI_TEXT_DIM);

    y = 312;
    for (int axis = 0; axis < 3; axis++)
    {
        int32_t mn, mx;
        const int bx = 132, bw = 170;
        const int bh2 = 8;

        switch (axis)
        {
        case 0: mn = mag->calib_min_x; mx = mag->calib_max_x; break;
        case 1: mn = mag->calib_min_y; mx = mag->calib_max_y; break;
        default: mn = mag->calib_min_z; mx = mag->calib_max_z; break;
        }
        cov = (mx > mn) ? (float)(mx - mn) / 2000.0f : 0.0f;
        if (cov > 1.0f) cov = 1.0f;

        ui_text_scaled(rs->cbuf, rs->buf_width, rs->buf_height,
                       96, y - 3,
                       axis == 0 ? "X" : (axis == 1 ? "Y" : "Z"),
                       2, cov >= 1.0f ? UI_ACC_LIME : UI_ACC_ORANGE);
        ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height,
                   bx, y, bw, bh2, UI_TRACK);
        if (cov > 0.02f)
            ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height,
                       bx, y, (int)(bw * cov) > bh2 ? (int)(bw * cov) : bh2,
                       bh2, cov >= 1.0f ? UI_ACC_LIME : UI_ACC_ORANGE);
        y += 28;
    }
    label_center(rs->cbuf, rs->buf_width, 396,
                 "KEEP AWAY FROM PHONE / METAL", UI_TEXT_FAINT);
}

/* ------------------------------------------------------------------ *
 * page render dispatch
 * ------------------------------------------------------------------ */

static bool map_active(const app_ctx_t *ctx)
{
    return ctx->ui.current == PAGE_ROUTE ||
           (ctx->ui.current == PAGE_RUN && ctx->ui.run_map_view);
}

/* wrap the notification text into up to three 26-character lines;
 * returns the number of lines used (0 when there is nothing to show) */
static int ai_card_wrap(ai_agent_t *ai, char line[3][28])
{
    const int maxc = 26;
    const char *t;
    int n, i, k;

    memset(line, 0, 3 * 28);
    if (!ai || !ai->active || !ai->notify[0])
        return 0;

    t = ai->notify;
    n = (int)strlen(t);
    i = 0;
    for (k = 0; k < 3 && i < n; k++)
    {
        int take = n - i;
        if (take > maxc)
        {
            take = maxc;
            while (take > 0 && t[i + take] != ' ')
                take--;
            if (take <= 0)
                take = maxc;
        }
        memcpy(line[k], t + i, (size_t)take);
        line[k][take] = '\0';
        i += take;
        while (t[i] == ' ')
            i++;
    }
    return k;
}

/* card grows upwards from a fixed bottom edge; the height follows the
 * number of wrapped lines so a one-line answer stays a compact toast */
#define AI_CARD_BOTTOM 420

static void ai_card_geom(int nlines, int *y, int *h)
{
    if (nlines < 1) nlines = 1;
    if (nlines > 3) nlines = 3;
    *h = 60 + 20 * nlines;
    *y = AI_CARD_BOTTOM - *h;
}

/* AI notification card (proactive message / LLM answer / tool result) */
static void draw_ai_card(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    ai_agent_t *ai = ai_agent_get();
    char line[3][28];
    int nlines, k, y, h;
    const int cx0 = SAFE_X;
    const int cw = SCREEN_W - 2 * SAFE_X;

    (void)now_ms;
    nlines = ai_card_wrap(ai, line);
    if (nlines == 0)
        return;
    ai_card_geom(nlines, &y, &h);

    ui_card_top(rs->cbuf, rs->buf_width, rs->buf_height,
                cx0, y, cw, h, 18, UI_CARD, UI_CARD_EDGE);
    /* accent edge + source tag (hints that a tap dismisses it) */
    ui_capsule(rs->cbuf, rs->buf_width, rs->buf_height,
               cx0 + 12, y + 14, 4, h - 28, UI_ACC_BLUE);
    label_at(rs->cbuf, rs->buf_width, cx0 + 28, y + 12,
             ai->notify_src[0] ? ai->notify_src : "AI", UI_ACC_BLUE);

    for (k = 0; k < nlines; k++)
        label_at(rs->cbuf, rs->buf_width, cx0 + 28,
                 y + 38 + k * 20, line[k], UI_TEXT);
}

int page_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;

    (void)rs;

    if (map_active(ctx))
    {
        page_run_map_render(ctx, now_ms);
        draw_calib_overlay(ctx, now_ms);
        draw_ai_card(ctx, now_ms);
        return 1;
    }

    if (!ctx->ui.dirty)
        return 0;

    switch (ctx->ui.current)
    {
    case PAGE_WATCH:
        page_watch_render(ctx, now_ms);
        break;
    case PAGE_RUN:
        page_run_data_render(ctx, now_ms);
        break;
    case PAGE_COMPASS:
        page_compass_render(ctx, now_ms);
        break;
    case PAGE_STATS:
        page_stats_render(ctx, now_ms);
        break;
    case PAGE_SETTINGS:
        page_settings_render(ctx, now_ms);
        break;
    default:
        break;
    }

    ctx->ui.dirty = false;
    page_slide_out(ctx, now_ms);
    draw_calib_overlay(ctx, now_ms);
    draw_ai_card(ctx, now_ms);
    return 1;
}

/* ------------------------------------------------------------------ *
 * touch routing: NO page changes from touch, NO long-press; only the
 * centre-line buttons (or view helpers on the map) react to a tap.
 * Navigation is exclusively via KEY1 (prev) / KEY2 (next) in main.c.
 * ------------------------------------------------------------------ */

static void tap_calibrate(app_ctx_t *ctx)
{
    if (ctx->sensors && !ctx->sensors->mag.calib_running)
    {
        sensor_calibrate_magnetometer(ctx->sensors, CALIB_MS);
        ctx->calib_end_ms = ctx->now_ms + CALIB_MS;
        ctx->calib_dur_ms = CALIB_MS;
        ctx->ui.dirty = true;
    }
}

void page_handle_touch(app_ctx_t *ctx, touch_event_t ev)
{
    render_state_t *rs = ctx->renderer;
    int tx = -1, ty = -1;

    if (ev != TOUCH_EV_TAP)
        return;                     /* no long press / double tap actions */

    touch_get_last_pos(&tx, &ty);

    /* the AI toast is modal: a tap on it dismisses the card instead of
     * falling through to whatever button sits underneath */
    {
        ai_agent_t *ai = ai_agent_get();
        char line[3][28];
        int nlines = ai_card_wrap(ai, line);
        if (nlines > 0)
        {
            int y, h;
            ai_card_geom(nlines, &y, &h);
            if (ty >= y && ty < y + h)
            {
                ai_agent_dismiss();
                return;
            }
        }
    }

    switch (ctx->ui.current)
    {
    case PAGE_WATCH:
        break;                      /* nothing touchable; keys navigate */

    case PAGE_RUN:
        if (ctx->ui.run_map_view)
        {
            if (btn_hit(tx, ty, SCREEN_W / 2, 344, BTN_W1))
            {
                ctx->ui.run_map_view = false;
                ctx->ui.dirty = true;
            }
            break;
        }

        /* data view centre buttons */
        switch (ctx->run.state)
        {
        case RUN_RUNNING:
            if (btn_hit(tx, ty, 112, RUN_BTN_Y, BTN_W2))
            {
                run_pause(&ctx->run);
                ctx->ui.dirty = true;
            }
            else if (btn_hit(tx, ty, 278, RUN_BTN_Y, BTN_W2))
            {
                stop_run(ctx);
            }
            break;
        case RUN_PAUSED:
            if (btn_hit(tx, ty, 112, RUN_BTN_Y, BTN_W2))
            {
                run_resume(&ctx->run);
                ctx->ui.dirty = true;
            }
            else if (btn_hit(tx, ty, 278, RUN_BTN_Y, BTN_W2))
            {
                stop_run(ctx);
            }
            break;
        case RUN_FINISHED:
            if (btn_hit(tx, ty, SCREEN_W / 2, RUN_BTN_Y, BTN_W1))
            {
                start_run(ctx);
            }
            break;
        default:
            if (btn_hit(tx, ty, SCREEN_W / 2, RUN_BTN_Y, BTN_W1))
            {
                start_run(ctx);
            }
            break;
        }
        break;

    case PAGE_ROUTE:
        /* map view helpers (no page navigation by touch) */
        if (btn_hit(tx, ty, SCREEN_W / 2, 344, BTN_W1))
        {
            ctx->ui.run_map_view = false;
            page_set(ctx, PAGE_RUN);
        }
        else if (ctx->run.state == RUN_RUNNING ||
                 ctx->run.state == RUN_PAUSED ||
                 (ctx->run.state == RUN_FINISHED &&
                  ctx->run.track_count >= 2))
        {
            renderer_reset_live_fit(rs);
            ctx->ui.dirty = true;
        }
        else if (rs->gpx && rs->gpx->loaded)
        {
            renderer_animate_to(rs, rs->gpx->center_lat,
                                rs->gpx->center_lon, ZOOM_DEFAULT, 300);
            ctx->ui.dirty = true;
        }
        break;

    case PAGE_COMPASS:
        if (btn_hit(tx, ty, SCREEN_W / 2, CMP_BTN_Y, BTN_W1))
            tap_calibrate(ctx);
        break;

    case PAGE_STATS:
        break;

    case PAGE_SETTINGS:
        if (ty >= 44 && ty < 100)
        {
            if (ctx->brightness_mode == 0)
                ctx->brightness_mode = 40;
            else if (ctx->brightness_mode == 40)
                ctx->brightness_mode = 70;
            else if (ctx->brightness_mode == 70)
                ctx->brightness_mode = 100;
            else
                ctx->brightness_mode = 0;
            kv_set_int("settings.brightness", ctx->brightness_mode);
            ctx->ui.dirty = true;
        }
        else if (ty >= 100 && ty < 156)
        {
            ctx->use_miles = !ctx->use_miles;
            kv_set_int("unit.miles", ctx->use_miles ? 1 : 0);
            ctx->ui.dirty = true;
        }
        else if (ty >= 156 && ty < 212)
        {
            if (ctx->decl_deg >= 20)
                ctx->decl_deg = -20;
            else
                ctx->decl_deg++;
            kv_set_int("mag.decl", ctx->decl_deg);
            ctx->ui.dirty = true;
        }
        else if (ty >= 212 && ty < 268)
        {
            tap_calibrate(ctx);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ *
 * per-frame update
 * ------------------------------------------------------------------ */

void page_tick(app_ctx_t *ctx, uint32_t now_ms)
{
    bool calibrating = ctx->sensors && ctx->sensors->mag.calib_running;

    ctx->now_ms = now_ms;

    if (ctx->ui.animating && ctx->ui.anim_ms != 0 &&
        now_ms - ctx->ui.anim_ms > 750)
    {
        ctx->ui.animating = false;
    }

    if (ctx->ui.animating)
        ctx->ui.dirty = true;

    if (calibrating)
        ctx->ui.dirty = true;
    else if (ctx->calib_end_ms && now_ms >= ctx->calib_end_ms &&
             now_ms < ctx->calib_end_ms + 500)
    {
        ctx->ui.dirty = true;
    }
    else if (ctx->calib_end_ms && now_ms >= ctx->calib_end_ms + 500)
    {
        ctx->calib_end_ms = 0;
        ctx->calib_dur_ms = 0;
    }

    if (ctx->run.state == RUN_RUNNING)
    {
        float amag = 9.81f;
        float gz = 0.0f;
        if (ctx->sensors && ctx->sensors->imu.present)
        {
            amag = ctx->sensors->imu.ax * ctx->sensors->imu.ax +
                   ctx->sensors->imu.ay * ctx->sensors->imu.ay +
                   ctx->sensors->imu.az * ctx->sensors->imu.az;
            amag = sqrtf(amag);
            gz = ctx->sensors->gyro_yaw_rate;
        }
#ifdef CONFIG_ARCH_SIM
        if (ctx->run.running_ms > 8000 && ctx->run.running_ms < 20000)
            gz = 18.0f;
#endif
        run_tick(&ctx->run, now_ms, amag, gz);
        ctx->today_steps = (int32_t)ctx->run.steps;
        ctx->ui.dirty = true;
    }

    if (ctx->run.state == RUN_FINISHED && !ctx->run_saved)
    {
        if (ctx->run.distance_m > 1.0f)
        {
            kv_history_push(ctx->run.distance_m, ctx->run.running_ms / 1000,
                            ctx->run.ascent_m);
            ctx->run_saved = true;
        }
        ctx->ui.dirty = true;
    }

    if (ai_agent_get()->active)
    {
        ctx->ui.dirty = true;      /* let the AI card time out */
    }
    else if (ctx->ui.current == PAGE_COMPASS)
    {
        ctx->ui.dirty = true;
    }
    else if (map_active(ctx))
    {
        ctx->ui.dirty = true;
    }
    else if (ctx->ui.page_start_ms == 0 ||
             now_ms - ctx->ui.page_start_ms >= 1000)
    {
        ctx->ui.page_start_ms = now_ms;
        ctx->ui.dirty = true;
    }
}
