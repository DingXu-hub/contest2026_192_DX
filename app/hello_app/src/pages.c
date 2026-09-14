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

static void text2_center(pixel_t *buf, int bw, int y, const char *s,
                         pixel_t c)
{
    int w = (int)strlen(s) * 6 * TXT2;
    ui_text_scaled(buf, bw, SCREEN_H, (bw - w) / 2, y, s, TXT2, c);
}

static void text2_at(pixel_t *buf, int bw, int x, int y, const char *s,
                     pixel_t c)
{
    ui_text_scaled(buf, bw, SCREEN_H, x, y, s, TXT2, c);
}

static void text2_at_center(pixel_t *buf, int bw, int bh, int cx, int y,
                            const char *s, pixel_t c)
{
    int w = (int)strlen(s) * 6 * TXT2;
    (void)bh;
    ui_text_scaled(buf, bw, SCREEN_H, cx - w / 2, y, s, TXT2, c);
}

/* WARNING: bw here is the REAL framebuffer width (pixel-row stride).
 * It must be the actual buffer width - never a layout/column width,
 * otherwise glyphs are written with the wrong stride and scatter across
 * the screen.  For centred columns use num_at_center()/text_at_center(). */
static void center_text(pixel_t *buf, int bw, int y, const char *s,
                        pixel_t c)
{
    int len = (int)strlen(s);
    ui_text(buf, bw, SCREEN_H, (bw - len * 6) / 2, y, s, c);
}

/* width of a 7-seg string (':' and '.' advance 5*scale, digits 9*scale+2) */
static int num_w(const char *num, int scale)
{
    int w = 0;
    while (*num)
    {
        if (*num == ':' || *num == '.')
            w += 5 * scale;
        else
            w += 9 * scale + 2;
        num++;
    }
    return w;
}

/* draw a 7-seg number horizontally centred on the real buffer width */
static void center_number(pixel_t *buf, int bw, int y, const char *num,
                          int scale, pixel_t c)
{
    int w = num_w(num, scale);
    int x = (bw - w) / 2;
    if (x < 0) x = 0;
    ui_number(buf, bw, SCREEN_H, x, y, num, scale, c);
}

/* 7-seg number / 5x7 text centred on an arbitrary x centre (buf_w MUST
 * stay the real framebuffer width - it is the pixel-row stride). */
static void num_at_center(pixel_t *buf, int buf_w, int buf_h,
                          int cx, int y, const char *num,
                          int scale, pixel_t c)
{
    int w = num_w(num, scale);
    ui_number(buf, buf_w, buf_h, cx - w / 2, y, num, scale, c);
}

static void text_at_center(pixel_t *buf, int buf_w, int buf_h,
                           int cx, int y, const char *s, pixel_t c)
{
    ui_text(buf, buf_w, buf_h, cx - (int)strlen(s) * 6 / 2, y, s, c);
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

/* centre-line action button: filled rounded rect with a coloured label */
static void draw_btn(render_state_t *rs, int cx, int y, int w,
                     const char *s, pixel_t c)
{
    int x = cx - w / 2;
    ui_card(rs->cbuf, rs->buf_width, rs->buf_height,
            x, y, w, BTN_H, 12, UI_CARD);
    ui_rrect(rs->cbuf, rs->buf_width, rs->buf_height,
             x, y, w, BTN_H, 12, c);
    text2_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                    cx, y + 16, s, c);
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
    float anim = page_anim(ctx, now_ms, 700);
    float ring_frac;

    /* The board has no RTC: after a cold boot the clock reads 1970.  For
     * demo purposes nudge it onto a fixed "today" once (this only shifts
     * the display; it does not set the system clock). */
    if (demo_off == 0)
        demo_off = 1782907200 - t;      /* 2026-07-01 12:00 UTC */
    t += demo_off;
    tm = localtime(&t);

    clear_screen(rs, UI_BG);

    if (!tm)
        return;

    snprintf(buf, sizeof(buf), "%s  %s %02d",
             wdays[tm->tm_wday], mons[tm->tm_mon], tm->tm_mday);
    text2_center(rs->cbuf, SCREEN_W, 42, buf, UI_TEXT_DIM);

    snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    center_number(rs->cbuf, SCREEN_W, 92, buf, 7, UI_TEXT);

    int goal = 8000;
    if (goal < 1) goal = 1;
    ring_frac = (float)ctx->today_steps / (float)goal;
    if (ring_frac > 1.0f) ring_frac = 1.0f;
    ring_frac *= anim;

    ui_ring_sport(rs->cbuf, rs->buf_width, rs->buf_height,
                  SCREEN_W / 2, 306, 54, ring_frac,
                  UI_ACCENT, UI_CARD_EDGE, 16);

    snprintf(buf, sizeof(buf), "%ld", (long)ctx->today_steps);
    center_number(rs->cbuf, SCREEN_W, 286, buf, 3, UI_TEXT);
    text2_center(rs->cbuf, SCREEN_W, 326, "STEPS", UI_TEXT_DIM);
    snprintf(buf, sizeof(buf), "GOAL %d", goal);
    text2_center(rs->cbuf, SCREEN_W, 362, buf, UI_TEXT_FAINT);

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

    text2_center(rs->cbuf, SCREEN_W, 16, run_state_str(re->state), col);

    /* hero: elapsed time */
    run_format_elapsed(re, buf, sizeof(buf));
    center_number(rs->cbuf, SCREEN_W, 76, buf, 5, col);

    /* distance */
    run_format_distance(re, buf, sizeof(buf), ctx->use_miles);
    center_number(rs->cbuf, SCREEN_W, 164, buf, 4, UI_TEXT);
    snprintf(buf, sizeof(buf), "DIST  %s", ctx->use_miles ? "MI" : "KM");
    text2_center(rs->cbuf, SCREEN_W, 214, buf, UI_TEXT_DIM);

    /* pace + cadence */
    {
        char pace[24];
        run_format_pace(re, pace, sizeof(pace), ctx->use_miles);
        snprintf(buf, sizeof(buf), "PACE %s    CAD %d",
                 pace, (int)re->cadence_spm);
        text2_center(rs->cbuf, SCREEN_W, 246, buf, UI_TEXT_FAINT);
    }

    /* progress to the run goal */
    frac = re->distance_m / RUN_GOAL_M;
    if (frac > 1.0f) frac = 1.0f;
    if (frac < 0.0f) frac = 0.0f;
    ui_bar(rs->cbuf, rs->buf_width, rs->buf_height,
           SAFE_X, 286, SCREEN_W - 2 * SAFE_X, 8, frac, col,
           UI_CARD_EDGE);
    if (ctx->use_miles)
        snprintf(buf, sizeof(buf), "GOAL %.1f MI", RUN_GOAL_M / 1609.344f);
    else
        snprintf(buf, sizeof(buf), "GOAL %.1f KM", RUN_GOAL_M / 1000.0f);
    text2_center(rs->cbuf, SCREEN_W, 304, buf, UI_TEXT_FAINT);

    /* centre action buttons */
    switch (re->state)
    {
    case RUN_RUNNING:
        draw_btn(rs, 112, RUN_BTN_Y, BTN_W2, "PAUSE",
                 UI_ACCENT_ORANGE);
        draw_btn(rs, 278, RUN_BTN_Y, BTN_W2, "STOP", UI_ACCENT_RED);
        break;
    case RUN_PAUSED:
        draw_btn(rs, 112, RUN_BTN_Y, BTN_W2, "RESUME", UI_ACCENT);
        draw_btn(rs, 278, RUN_BTN_Y, BTN_W2, "STOP", UI_ACCENT_RED);
        break;
    case RUN_FINISHED:
        draw_btn(rs, SCREEN_W / 2, RUN_BTN_Y, BTN_W1, "RUN AGAIN",
                 UI_ACCENT_RED);
        break;
    default:
        draw_btn(rs, SCREEN_W / 2, RUN_BTN_Y, BTN_W1, "START",
                 UI_ACCENT);
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


    /* status line + centre BACK button */
    if (trail)
    {
        run_format_distance(&ctx->run, buf, sizeof(buf), ctx->use_miles);
        {
            char tmp[32], tmp2[24];
            strncpy(tmp2, buf, sizeof(tmp2) - 1);
            tmp2[sizeof(tmp2) - 1] = '\0';
            run_format_elapsed(&ctx->run, tmp, sizeof(tmp));
            snprintf(buf, sizeof(buf), "%s %s   %s",
                     tmp2, ctx->use_miles ? "mi" : "km", tmp);
        }
        text2_center(rs->cbuf, SCREEN_W, 324, buf, UI_TEXT_DIM);
    }
    else if (ctx->gpx && ctx->gpx->loaded)
    {
        snprintf(buf, sizeof(buf), "ROUTE %.1f%s",
                 ctx->gpx->total_distance_m / 1000.0,
                 ctx->use_miles ? "mi" : "km");
        text2_center(rs->cbuf, SCREEN_W, 324, buf, UI_TEXT_DIM);
    }
    else
    {
        text2_center(rs->cbuf, SCREEN_W, 324, "NO ROUTE",
                    UI_TEXT_DIM);
    }
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

#define CMP_BTN_Y 344

/* keep the dial inside the rounded-corner safe area: with cy=212/R=122 the
 * ring spans y 90..334, the button 344..390 and the top text 16..54 */
static void page_compass_render(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    sensor_manager_t *sen = ctx->sensors;
    static const char *const card[4] = { "N", "E", "S", "W" };
    char buf[24];
    const int cx = SCREEN_W / 2, cy = 212, R = 122;
    float hdg;
    bool mag_ok;
    int a, deg;

    (void)now_ms;

    clear_screen(rs, UI_BG);

    /* magnetic source state (top centre) */
    mag_ok = (sen && sen->mag.present && sen->mag.healthy &&
              !sen->mag_dropped);
    if (sen && !sen->mag.present)
        text2_center(rs->cbuf, SCREEN_W, 16, "NO MAG", UI_TEXT_DIM);
    else if (mag_ok)
        text2_center(rs->cbuf, SCREEN_W, 16, "MAGNETIC", UI_ACCENT);
    else
        text2_center(rs->cbuf, SCREEN_W, 16, "GYRO ONLY", UI_TEXT_FAINT);

    /* tilt readout (top centre, single line) */
    if (sen && sen->imu.present)
    {
        snprintf(buf, sizeof(buf), "P %+03d   R %+03d",
                 (int)sen->imu.pitch, (int)sen->imu.roll);
        text2_center(rs->cbuf, SCREEN_W, 40, buf, UI_TEXT_DIM);
    }

    hdg = compass_smooth(ctx,
                         (sen ? sen->heading_deg : 0.0f) +
                         (float)ctx->decl_deg);
    while (hdg >= 360.0f) hdg -= 360.0f;
    while (hdg < 0.0f) hdg += 360.0f;
    deg = (int)(hdg + 0.5f) % 360;

    /* bezel */
    ui_ring(rs->cbuf, rs->buf_width, rs->buf_height,
            cx, cy, R + 1, 1.0f, UI_CARD_EDGE, UI_BG);
    ui_ring(rs->cbuf, rs->buf_width, rs->buf_height,
            cx, cy, R + 2, 1.0f, UI_CARD_EDGE, UI_BG);

    /* rotating dial */
    for (a = 0; a < 360; a += 30)
    {
        float rel = ((float)a - hdg) * (float)M_PI / 180.0f;
        float sn = sinf(rel), cs = cosf(rel);
        bool cardinal = (a % 90) == 0;
        int  len = cardinal ? 26 : 13;
        int  x1 = cx + (int)((R - 2) * sn);
        int  y1 = cy - (int)((R - 2) * cs);
        int  x2 = cx + (int)((R - 2 - len) * sn);
        int  y2 = cy - (int)((R - 2 - len) * cs);

        px_line(rs->cbuf, rs->buf_width, rs->buf_height,
                x1, y1, x2, y2,
                cardinal ? UI_TEXT : UI_TEXT_FAINT);

        if (cardinal)
        {
            int lx = cx + (int)((R - 44) * sn) - 3;
            int ly = cy - (int)((R - 44) * cs) - 4;
            ui_text(rs->cbuf, rs->buf_width, rs->buf_height,
                    lx, ly, card[a / 90],
                    (a == 0) ? PIXEL_RED : UI_TEXT);
        }
    }

    /* inner hub */
    ui_ring(rs->cbuf, rs->buf_width, rs->buf_height,
            cx, cy, 72, 1.0f, 0x18E3, UI_BG);

    /* fixed red lubber pointer at 12 o'clock */
    {
        int y;
        for (y = 0; y < 18; y++)
        {
            int hw = 8 - (8 * y) / 17;
            int yy = cy - R - 22 + y;
            int x;
            if (yy < 0 || yy >= SCREEN_H)
                continue;
            for (x = cx - hw; x <= cx + hw; x++)
                if (x >= 0 && x < SCREEN_W)
                    rs->cbuf[yy * SCREEN_W + x] = PIXEL_RED;
        }
    }

    /* big bearing readout */
    snprintf(buf, sizeof(buf), "%03d", deg);
    center_number(rs->cbuf, SCREEN_W, cy - 36, buf, 5, UI_TEXT);

    draw_btn(rs, SCREEN_W / 2, CMP_BTN_Y, BTN_W1, "CALIBRATE",
             UI_ACCENT_ORANGE);

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
    const int colw = (SCREEN_W - 2 * SAFE_X) / 3;
    const int c1 = SAFE_X + colw / 2;
    const int c2 = SAFE_X + colw + colw / 2;
    const int c3 = SAFE_X + 2 * colw + colw / 2;

    (void)now_ms;
    clear_screen(rs, UI_BG);

    text2_center(rs->cbuf, SCREEN_W, 20, "STATS", UI_TEXT_DIM);

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

    snprintf(buf, sizeof(buf), "%d", runs);
    num_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                  c1, 60, buf, 3, UI_ACCENT_BLUE);
    text2_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                    c1, 96, "RUNS", UI_TEXT_DIM);

    if (ctx->use_miles)
        snprintf(buf, sizeof(buf), "%.1f", total_d / 1609.344f);
    else
        snprintf(buf, sizeof(buf), "%.1f", total_d / 1000.0f);
    num_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                  c2, 60, buf, 3, UI_ACCENT);
    text2_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                    c2, 96, ctx->use_miles ? "MI" : "KM", UI_TEXT_DIM);

    snprintf(buf, sizeof(buf), "%lu", (unsigned long)(total_s / 60));
    num_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                  c3, 60, buf, 3, UI_ACCENT_ORANGE);
    text2_at_center(rs->cbuf, rs->buf_width, rs->buf_height,
                    c3, 96, "MIN", UI_TEXT_DIM);

    ui_hline(rs->cbuf, rs->buf_width, rs->buf_height,
             SAFE_X, 128, SCREEN_W - 2 * SAFE_X, UI_CARD_EDGE);
    text2_center(rs->cbuf, SCREEN_W, 140, "RECENT", UI_TEXT_FAINT);

    if (n == 0)
    {
        text2_center(rs->cbuf, SCREEN_W, 230, "NO RUNS YET", UI_TEXT_DIM);
        ui_dots(rs->cbuf, rs->buf_width, rs->buf_height, PAGE_STATS);
        return;
    }

    {
        int y = 176;
        int shown = 0;
        for (i = 0; i < n && shown < 5 && y + 14 < SCREEN_H - CORNER_R; i++, shown++)
        {
            float d; uint32_t sec; float asc;
            if (kv_history_get(i, &d, &sec, &asc) < 0)
                continue;

            snprintf(buf, sizeof(buf), "#%02d", i + 1);
            text2_at(rs->cbuf, rs->buf_width, 32, y, buf, UI_TEXT_FAINT);

            snprintf(buf, sizeof(buf), "%.2f%s",
                     ctx->use_miles ? d / 1609.344f : d / 1000.0f,
                     ctx->use_miles ? "mi" : "km");
            text2_at(rs->cbuf, rs->buf_width, 84, y, buf, UI_TEXT);

            snprintf(buf, sizeof(buf), "%02lu:%02lu",
                     (unsigned long)(sec / 60), (unsigned long)(sec % 60));
            text2_at(rs->cbuf, rs->buf_width,
                     358 - (int)strlen(buf) * 12, y, buf, UI_TEXT_DIM);

            y += 34;
            if (shown < n - 1 && shown < 4)
                ui_hline(rs->cbuf, rs->buf_width, rs->buf_height,
                         32, y - 4, SCREEN_W - 64, 0x18E3);
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
    const int x0 = 32;
    const int row_h = 56;
    const int y0 = 44;
    const int xr = 358;      /* right edge of values (corner-safe) */
    int i;

    (void)now_ms;

    clear_screen(rs, UI_BG);
    text2_center(rs->cbuf, SCREEN_W, 20, "SETTINGS", UI_TEXT_DIM);

    for (i = 0; i < 4; i++)
    {
        if (i)
            ui_hline(rs->cbuf, rs->buf_width, rs->buf_height,
                     x0, y0 + i * row_h - 4,
                     SCREEN_W - 2 * x0, UI_CARD_EDGE);
    }

    /* brightness */
    text2_at(rs->cbuf, rs->buf_width, x0, y0 + 18, "BRIGHTNESS", UI_TEXT);
    if (ctx->brightness_mode == 0)
        text2_at(rs->cbuf, rs->buf_width, xr - 4 * 12, y0 + 18, "AUTO",
                 UI_ACCENT);
    else
    {
        snprintf(buf, sizeof(buf), "%d%%", ctx->brightness_mode);
        text2_at(rs->cbuf, rs->buf_width,
                 xr - (int)strlen(buf) * 12, y0 + 18, buf, UI_ACCENT);
    }

    /* units */
    text2_at(rs->cbuf, rs->buf_width, x0, y0 + row_h + 18, "UNITS",
             UI_TEXT);
    text2_at(rs->cbuf, rs->buf_width, xr - 2 * 12, y0 + row_h + 18,
             ctx->use_miles ? "MI" : "KM", UI_ACCENT_BLUE);

    /* declination */
    text2_at(rs->cbuf, rs->buf_width, x0, y0 + 2 * row_h + 18,
             "DECLINATION", UI_TEXT);
    snprintf(buf, sizeof(buf), "%+d", (int)ctx->decl_deg);
    text2_at(rs->cbuf, rs->buf_width, xr - (int)strlen(buf) * 12,
             y0 + 2 * row_h + 18, buf,
             ctx->decl_deg == 0 ? UI_TEXT_DIM : UI_ACCENT_ORANGE);

    /* compass calibration */
    text2_at(rs->cbuf, rs->buf_width, x0, y0 + 3 * row_h + 18,
             "COMPASS CAL", UI_TEXT);
    text2_at(rs->cbuf, rs->buf_width, xr - 3 * 12, y0 + 3 * row_h + 18,
             "TAP", UI_ACCENT_ORANGE);

    text2_center(rs->cbuf, SCREEN_W, y0 + 4 * row_h + 6,
                 "HUANGSHAN UI v3.3", UI_TEXT_FAINT);

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

    text2_center(rs->cbuf, SCREEN_W, 52, "CALIBRATE", UI_ACCENT_ORANGE);
    text2_center(rs->cbuf, SCREEN_W, 78,
                 "FIGURE-8 + TUMBLE ALL AXES", UI_TEXT_DIM);

    left_ms = (mag->calib_end_ms > now_ms) ? mag->calib_end_ms - now_ms : 0;
    if (ctx->calib_dur_ms)
    {
        frac = 1.0f - (float)left_ms / (float)ctx->calib_dur_ms;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
    }
    else
        frac = 0.0f;

    ui_ring_sport(rs->cbuf, rs->buf_width, rs->buf_height,
                  SCREEN_W / 2, 204, 60, frac,
                  UI_ACCENT_ORANGE, UI_CARD_EDGE, 20);

    snprintf(buf, sizeof(buf), "%d", (int)((left_ms + 999) / 1000));
    center_number(rs->cbuf, SCREEN_W, 184, buf, 4, UI_TEXT);
    text2_center(rs->cbuf, SCREEN_W, 232, "SECONDS LEFT", UI_TEXT_DIM);

    y = 288;
    for (int axis = 0; axis < 3; axis++)
    {
        int32_t mn, mx;
        int bx = 126, bw = 170;

        switch (axis)
        {
        case 0: mn = mag->calib_min_x; mx = mag->calib_max_x; break;
        case 1: mn = mag->calib_min_y; mx = mag->calib_max_y; break;
        default: mn = mag->calib_min_z; mx = mag->calib_max_z; break;
        }
        cov = (mx > mn) ? (float)(mx - mn) / 2000.0f : 0.0f;
        if (cov > 1.0f) cov = 1.0f;

        ui_text_scaled(rs->cbuf, rs->buf_width, rs->buf_height,
                       bx - 34, y - 3,
                       axis == 0 ? "X" : (axis == 1 ? "Y" : "Z"),
                       2, cov >= 1.0f ? UI_ACCENT : UI_ACCENT_ORANGE);
        ui_bar(rs->cbuf, rs->buf_width, rs->buf_height,
               bx, y, bw, 8, cov,
               cov >= 1.0f ? UI_ACCENT : UI_ACCENT_ORANGE, UI_CARD);
        y += 26;
    }
    text2_center(rs->cbuf, SCREEN_W, 368,
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

/* AI notification card (proactive message / LLM answer / tool result) */
static void draw_ai_card(app_ctx_t *ctx, uint32_t now_ms)
{
    render_state_t *rs = ctx->renderer;
    ai_agent_t *ai = ai_agent_get();
    char l1[28], l2[28];
    const char *t;
    int n, i, y;

    (void)now_ms;
    if (!ai || !ai->active || !ai->notify[0])
        return;

    /* wrap the text into up to two 26-character lines */
    t = ai->notify;
    n = (int)strlen(t);
    i = n < 26 ? n : 26;
    while (i > 0 && t[i] != ' ' && i < n)
        i--;
    if (i <= 0 || i >= n)
        i = n < 26 ? n : 26;
    memcpy(l1, t, (size_t)i);
    l1[i] = '\0';
    if (n > i)
    {
        int j = n - i - 1;
        if (j > 26) j = 26;
        memcpy(l2, t + i + 1, (size_t)j);
        l2[j] = '\0';
    }
    else
        l2[0] = '\0';

    y = SCREEN_H - 104;
    ui_card(rs->cbuf, rs->buf_width, rs->buf_height,
            SAFE_X, y, SCREEN_W - 2 * SAFE_X, 64, 12, UI_CARD);
    ui_rrect(rs->cbuf, rs->buf_width, rs->buf_height,
             SAFE_X, y, SCREEN_W - 2 * SAFE_X, 64, 12, UI_ACCENT_BLUE);

    text2_at(rs->cbuf, rs->buf_width, SAFE_X + 12, y + 6,
             ai->notify_src[0] ? ai->notify_src : "ai", UI_ACCENT_BLUE);
    text2_at(rs->cbuf, rs->buf_width, SAFE_X + 12, y + 24, l1, UI_TEXT);
    if (l2[0])
        text2_at(rs->cbuf, rs->buf_width, SAFE_X + 12, y + 44, l2,
                 UI_TEXT);
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
