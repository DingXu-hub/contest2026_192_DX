/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * route_renderer.c - see route_renderer.h for the architecture description.
 *
 * Rendering pipeline (GPU tile mode):
 *   render_tile():  bg + grid + route + markers into an unrotated square
 *                   tile (projection uses zoom/center only).
 *   per frame:      gpu_fill_rect(cbuf, BG) -> gpu_rotate_scale(tile -> cbuf)
 *                   -> CPU overlay (compass/scale/info).
 *
 * The tile is re-rendered only when pan/zoom/route/gesture-commit changes;
 * compass rotation alone reuses the cached tile and costs one GPU rotate.
 */

#include <nuttx/config.h>
#include <nuttx/cache.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#include "route_renderer.h"
#include "gpu.h"
#include "ui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.29577951308232
#endif

#define METERS_PER_DEG  111320.0

#define LINE_WIDTH_BASE  3
#define MARKER_RADIUS    9
#define COMPASS_RADIUS   22
#define SCALE_BAR_HEIGHT 6
#define ROTATION_FOLLOW  0.25f   /* smoothing for compass auto-rotation */
#define ROTATION_SETTLE_DEG 0.4f /* per-frame delta below which we idle */
#define ROTATION_SETTLE_FRAMES 6

/* ---- live run-track tuning (sport-watch style) ---- */
#define TRACK_SCALE_MIN     0.35f   /* px/m at full zoom-out            */
#define TRACK_SCALE_MAX     14.0f   /* px/m close-up at the start       */
#define TRACK_SCALE_INIT    14.0f   /* scale when a new run starts      */
#define TRACK_VIEW_FRAC     0.36f   /* trail fits inside this fraction  */
#define TRACK_FIT_SMOOTH    0.10f   /* per-frame lerp toward fit target */
#define TRACK_FADE_POINTS   40      /* last N points drawn full color   */
#define TRACK_FADE_MIX      0.55f   /* older trail mixed toward bg      */
#define TRACK_KM_DOT_R      2       /* split dot radius (px)            */
#define TRACK_MAX_DRAW_PTS  600     /* decimate longer trails for 60fps */

/* pace zones, seconds per km (Strava/Garmin style heat coloring) */
#define PACE_ZONE1_SPK  250.0f  /* <4:10 fast      */
#define PACE_ZONE2_SPK  300.0f  /* 4:10-5:00       */
#define PACE_ZONE3_SPK  360.0f  /* 5:00-6:00       */
#define PACE_ZONE4_SPK  430.0f  /* 6:00-7:10       */
                                 /* >7:10 slow      */

/* ------------------------------------------------------------------ *
 * color helpers
 * ------------------------------------------------------------------ */

/* 50/50-style channel mix between two RGB565 colors (for trail fade) */
static pixel_t mix565(pixel_t a, pixel_t b, float t)
{
    int ra = (a >> 11) & 0x1F, ga = (a >> 5) & 0x3F, ba = a & 0x1F;
    int rb = (b >> 11) & 0x1F, gb = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint8_t r = (uint8_t)(ra + (rb - ra) * t + 0.5f);
    uint8_t g = (uint8_t)(ga + (gb - ga) * t + 0.5f);
    uint8_t bv = (uint8_t)(ba + (bb - ba) * t + 0.5f);
    return (pixel_t)((r << 11) | (g << 5) | bv);
}

/* pace-zone color: green (fast) -> lime -> amber -> orange -> red (slow) */
pixel_t renderer_pace_color(float spk)
{
    if (spk <= 0.0f)
        return pixel_rgb(58, 110, 165);              /* unknown: steel blue */
    if (spk < PACE_ZONE1_SPK)
        return pixel_rgb(46, 229, 157);              /* mint green  */
    if (spk < PACE_ZONE2_SPK)
        return pixel_rgb(168, 217, 72);              /* lime        */
    if (spk < PACE_ZONE3_SPK)
        return pixel_rgb(247, 183, 49);              /* amber       */
    if (spk < PACE_ZONE4_SPK)
        return pixel_rgb(253, 150, 68);              /* orange      */
    return pixel_rgb(252, 92, 101);                  /* red         */
}

/* ------------------------------------------------------------------ *
 * grid helpers
 * ------------------------------------------------------------------ */

/* "nice" grid cell in meters so a cell is 24..96 px at the given scale */
static float nice_grid_m(float scale_px_m)
{
    static const float cells[] = { 2, 5, 10, 25, 50, 100,
                                   250, 500, 1000, 2000, 5000 };
    int i;

    if (scale_px_m <= 0.0f)
        return 100.0f;
    for (i = 0; i < (int)(sizeof(cells) / sizeof(cells[0])); i++)
    {
        float px = cells[i] * scale_px_m;
        if (px >= 24.0f && px <= 96.0f)
            return cells[i];
    }
    /* outside the band: nearest extreme */
    return (cells[0] * scale_px_m > 96.0f) ? cells[0] : cells[10];
}

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static uint32_t get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec);
}

static inline float wrap_deg(float deg)
{
    while (deg >= 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static void draw_line(pixel_t *buf, int16_t buf_w, int16_t buf_h,
                      int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      pixel_t color, uint8_t width)
{
    int16_t dx = (int16_t)abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int16_t dy = (int16_t)-abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy, e2;
    int8_t  wy, wx;

    for (;;)
    {
        for (wy = -(int8_t)(width / 2); wy <= (int8_t)(width / 2); wy++)
        {
            for (wx = -(int8_t)(width / 2); wx <= (int8_t)(width / 2); wx++)
            {
                int16_t px = x0 + wx, py = y0 + wy;
                if (px < 0 || px >= buf_w || py < 0 || py >= buf_h) continue;
                buf[py * buf_w + px] = color;
            }
        }

        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_circle(pixel_t *buf, int16_t buf_w, int16_t buf_h,
                        int16_t cx, int16_t cy, int16_t r,
                        pixel_t color, bool fill)
{
    int16_t x = r, y = 0, err = 0;
    int16_t i;

    while (x >= y)
    {
        if (fill)
        {
            for (i = cx - x; i <= cx + x; i++)
                if (i >= 0 && i < buf_w && cy + y >= 0 && cy + y < buf_h)
                    buf[(cy + y) * buf_w + i] = color;
            for (i = cx - x; i <= cx + x; i++)
                if (i >= 0 && i < buf_w && cy - y >= 0 && cy - y < buf_h)
                    buf[(cy - y) * buf_w + i] = color;
            for (i = cx - y; i <= cx + y; i++)
                if (i >= 0 && i < buf_w && cy + x >= 0 && cy + x < buf_h)
                    buf[(cy + x) * buf_w + i] = color;
            for (i = cx - y; i <= cx + y; i++)
                if (i >= 0 && i < buf_w && cy - x >= 0 && cy - x < buf_h)
                    buf[(cy - x) * buf_w + i] = color;
        }
        else
        {
            if (cx + x >= 0 && cx + x < buf_w && cy + y >= 0 && cy + y < buf_h)
                buf[(cy + y) * buf_w + cx + x] = color;
            if (cx + y >= 0 && cx + y < buf_w && cy + x >= 0 && cy + x < buf_h)
                buf[(cy + x) * buf_w + cx + y] = color;
            if (cx - y >= 0 && cx - y < buf_w && cy + x >= 0 && cy + x < buf_h)
                buf[(cy + x) * buf_w + cx - y] = color;
            if (cx - x >= 0 && cx - x < buf_w && cy + y >= 0 && cy + y < buf_h)
                buf[(cy + y) * buf_w + cx - x] = color;
            if (cx - x >= 0 && cx - x < buf_w && cy - y >= 0 && cy - y < buf_h)
                buf[(cy - y) * buf_w + cx - x] = color;
            if (cx - y >= 0 && cx - y < buf_w && cy - x >= 0 && cy - x < buf_h)
                buf[(cy - x) * buf_w + cx - y] = color;
            if (cx + y >= 0 && cx + y < buf_w && cy - x >= 0 && cy - x < buf_h)
                buf[(cy - x) * buf_w + cx + y] = color;
            if (cx + x >= 0 && cx + x < buf_w && cy - y >= 0 && cy - y < buf_h)
                buf[(cy - y) * buf_w + cx + x] = color;
        }

        if (err <= 0) { y += 1; err += 2 * y + 1; }
        if (err > 0)  { x -= 1; err -= 2 * x + 1; }
    }
}

/* ------------------------------------------------------------------ *
 * projection
 * ------------------------------------------------------------------ */

/* pixels per degree at zoom=1.0 */

/* ------------------------------------------------------------------ *
 * projection
 * ------------------------------------------------------------------ */

/* pixels per degree at zoom=1.0 */
static double px_per_deg(render_state_t *rs)
{
    return METERS_PER_DEG * rs->viewport.zoom / 10.0;
}

void screen_to_geo(render_state_t *rs, int16_t sx, int16_t sy,
                   double *lat, double *lon)
{
    double ppd = px_per_deg(rs);
    float cx = rs->buf_width / 2.0f + rs->viewport.offset_x;
    float cy = rs->buf_height / 2.0f + rs->viewport.offset_y;
    float rad = rs->viewport.rotation_deg * (float)DEG_TO_RAD;
    float rx = sx - cx, ry = sy - cy;
    float ux = rx * cosf(rad) - ry * sinf(rad);
    float uy = rx * sinf(rad) + ry * cosf(rad);

    *lon = rs->viewport.center_lon + ux / ppd;
    *lat = rs->viewport.center_lat - uy / ppd;
}

void geo_to_screen(render_state_t *rs, double lat, double lon,
                   int16_t *sx, int16_t *sy)
{
    double ppd = px_per_deg(rs);
    float cx = rs->buf_width / 2.0f + rs->viewport.offset_x;
    float cy = rs->buf_height / 2.0f + rs->viewport.offset_y;
    float dx = (float)((lon - rs->viewport.center_lon) * ppd);
    float dy = (float)((lat - rs->viewport.center_lat) * ppd);
    float rad = rs->viewport.rotation_deg * (float)DEG_TO_RAD;
    float rx = dx * cosf(rad) - dy * sinf(rad);
    float ry = dx * sinf(rad) + dy * cosf(rad);

    *sx = (int16_t)(cx + rx);
    *sy = (int16_t)(cy - ry);
}

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */

int renderer_init(render_state_t *rs)
{
    memset(rs, 0, sizeof(render_state_t));

    rs->viewport.zoom = ZOOM_DEFAULT;
    rs->viewport.rotation_deg = 0.0f;
    rs->show_grid = true;
    rs->show_waypoints = true;
    rs->show_gpx = true;
    rs->show_elevation_profile = false;
    rs->needs_redraw = true;
    rs->buf_width = RENDER_WIDTH;
    rs->buf_height = RENDER_HEIGHT;
    rs->gesture_scale = 1.0f;
    rs->rotation_settled = false;

    /* Try to bring up the ePicasso GPU; fall back to software silently. */
    if (gpu_init() == 0)
    {
        rs->gpu_ok = true;
    }

    /* Static base tile (PSRAM heap): background + grid, built once. */
    rs->tile_edge = TILE_EDGE;
    rs->base_tile = (pixel_t *)malloc((size_t)rs->tile_edge * rs->tile_edge *
                                      sizeof(pixel_t));
    rs->base_tile_valid = false;

    return 0;
}

void renderer_set_gpx(render_state_t *rs, gpx_data_t *gpx)
{
    rs->gpx = gpx;
    rs->needs_redraw = true;
}

void renderer_set_zoom(render_state_t *rs, float zoom, int16_t cx, int16_t cy)
{
    double lat, lon;

    screen_to_geo(rs, cx, cy, &lat, &lon);

    if (zoom < ZOOM_MIN) zoom = ZOOM_MIN;
    if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;
    rs->viewport.zoom = zoom;
    rs->viewport.center_lat = lat;
    rs->viewport.center_lon = lon;
    rs->viewport.offset_x = 0;
    rs->viewport.offset_y = 0;

    rs->needs_redraw = true;
}

void renderer_pan(render_state_t *rs, int16_t dx, int16_t dy)
{
    /* screen-space pan: the projection anchors the map to the viewport
     * center and offsets the whole scene by offset_x/offset_y, so the
     * map visibly follows the finger (grid + route move together) */
    rs->viewport.offset_x += (float)dx;
    rs->viewport.offset_y += (float)dy;

    if (rs->viewport.offset_x > 5000.0f) rs->viewport.offset_x = 5000.0f;
    if (rs->viewport.offset_x < -5000.0f) rs->viewport.offset_x = -5000.0f;
    if (rs->viewport.offset_y > 5000.0f) rs->viewport.offset_y = 5000.0f;
    if (rs->viewport.offset_y < -5000.0f) rs->viewport.offset_y = -5000.0f;

    rs->needs_redraw = true;
}

void renderer_set_rotation(render_state_t *rs, float deg)
{
    float diff = wrap_deg(deg - rs->viewport.rotation_deg);
    float follow = ROTATION_FOLLOW;

    if (fabsf(diff) < 1.5f)
        follow = 1.0f;
    else if (fabsf(diff) < 8.0f)
        follow = 0.5f;

    /* Accumulate the smoothed delta and only repaint once the compass has
     * really moved.  Magnetometer noise then does not force a GPU rotate
     * every frame - the preview stays 0 fps while the heading is stable. */
    rs->rotation_pending += diff * follow;
    if (fabsf(rs->rotation_pending) < ROTATION_SETTLE_DEG)
        return;

    rs->viewport.rotation_deg =
        wrap_deg(rs->viewport.rotation_deg + rs->rotation_pending);
    rs->rotation_pending = 0.0f;
    rs->needs_redraw = true;
    rs->rotation_settled = false;
}

void renderer_center_on_point(render_state_t *rs, double lat, double lon)
{
    rs->viewport.center_lat = lat;
    rs->viewport.center_lon = lon;
    rs->viewport.offset_x = 0;
    rs->viewport.offset_y = 0;
    rs->needs_redraw = true;
}

void renderer_animate_to(render_state_t *rs, double lat, double lon,
                         float zoom, uint32_t duration_ms)
{
    rs->viewport.animating = true;
    rs->viewport.anim_from_lat = rs->viewport.center_lat;
    rs->viewport.anim_from_lon = rs->viewport.center_lon;
    rs->viewport.anim_to_lat = lat;
    rs->viewport.anim_to_lon = lon;
    rs->viewport.anim_from_zoom = rs->viewport.zoom;
    rs->viewport.anim_to_zoom = zoom;
    rs->viewport.anim_start_ms = get_time_ms();
    rs->viewport.anim_duration_ms = duration_ms ? duration_ms : 300;
    rs->needs_redraw = true;
}

void renderer_set_gesture_scale(render_state_t *rs, float scale)
{
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 4.0f) scale = 4.0f;
    rs->gesture_scale = scale;
    /* a pinch on the live track switches to manual zoom (auto-fit off) */
    if (rs->live_track_mode)
        rs->track_fit_auto = false;
    rs->needs_redraw = true;
}

void renderer_commit_gesture(render_state_t *rs)
{
    if (rs->gesture_scale != 1.0f)
    {
        if (rs->live_track_mode)
        {
            /* bake the pinch into the live track scale */
            float s = rs->track_scale * rs->gesture_scale;
            if (s < TRACK_SCALE_MIN) s = TRACK_SCALE_MIN;
            if (s > TRACK_SCALE_MAX) s = TRACK_SCALE_MAX;
            rs->track_manual_scale = s;
            rs->track_scale = s;
            rs->gesture_scale = 1.0f;
            rs->needs_redraw = true;
            return;
        }
        rs->viewport.zoom *= rs->gesture_scale;
        if (rs->viewport.zoom < ZOOM_MIN) rs->viewport.zoom = ZOOM_MIN;
        if (rs->viewport.zoom > ZOOM_MAX) rs->viewport.zoom = ZOOM_MAX;
        rs->gesture_scale = 1.0f;
        rs->needs_redraw = true;
    }
}

void renderer_force_redraw(render_state_t *rs)
{
    rs->needs_redraw = true;
}

/* enter/leave the live dead-reckoned track mode (route page, running) */
void renderer_set_live_track(render_state_t *rs, bool live)
{
    if (rs->live_track_mode == live)
        return;
    rs->live_track_mode = live;
    rs->needs_redraw = true;
    if (live)
        renderer_reset_live_fit(rs);
}

/* re-enable auto-fit: center + fresh close-up scale (double tap / new run) */
void renderer_reset_live_fit(render_state_t *rs)
{
    rs->track_fit_auto = true;
    rs->track_manual_scale = TRACK_SCALE_INIT;
    rs->track_scale = TRACK_SCALE_INIT;
    rs->gesture_scale = 1.0f;
    rs->needs_redraw = true;
}

/* ------------------------------------------------------------------ *
 * static base tile (background + grid, rebuilt rarely)
 * ------------------------------------------------------------------ */

static void build_base_tile(render_state_t *rs)
{
    uint32_t t0 = get_time_us();

    if (!rs->base_tile)
        return;

    /* background only: one ePicasso fill for the whole tile.  The grid
     * is drawn per-frame in screen space so it scrolls with panning. */
    gpu_fill_rect(rs->base_tile, rs->tile_edge, rs->tile_edge,
                  0, 0, rs->tile_edge - 1, rs->tile_edge - 1,
                  PIXEL_NAVY);

    /* the ePicasso engine reads the base tile over its own bus */
    up_clean_dcache((uintptr_t)rs->base_tile,
                    (uintptr_t)rs->base_tile +
                        (size_t)rs->tile_edge * rs->tile_edge * 2);

    rs->base_tile_valid = true;

    if (rs->frame_count < 3)
    {
        printf("[Base] build=%uus\\n", (unsigned)(get_time_us() - t0));
    }
}

/* ------------------------------------------------------------------ *
 * frame update
 * ------------------------------------------------------------------ */

int renderer_update(render_state_t *rs, uint32_t now_ms)
{
    uint32_t t0 = get_time_us();

    /* Animation progress */
    if (rs->viewport.animating)
    {
        uint32_t elapsed = now_ms - rs->viewport.anim_start_ms;
        float t = (float)elapsed / (float)rs->viewport.anim_duration_ms;
        if (t >= 1.0f)
        {
            t = 1.0f;
            rs->viewport.animating = false;
        }
        float eased = t * t * (3.0f - 2.0f * t); /* smoothstep */
        rs->viewport.center_lat = rs->viewport.anim_from_lat +
            (rs->viewport.anim_to_lat - rs->viewport.anim_from_lat) * eased;
        rs->viewport.center_lon = rs->viewport.anim_from_lon +
            (rs->viewport.anim_to_lon - rs->viewport.anim_from_lon) * eased;
        rs->viewport.zoom = rs->viewport.anim_from_zoom +
            (rs->viewport.anim_to_zoom - rs->viewport.anim_from_zoom) * eased;
        rs->needs_redraw = true;
    }

    /* Skip completely when nothing changed (idle): this is the main
     * power win - a settled preview renders zero frames per second.
     * The live page forces needs_redraw itself while the trail grows. */
    if (!rs->needs_redraw)
    {
        rs->frame_count++;
        rs->current_fps = 0.0f;
        return 0;
    }

    if (!rs->cbuf)
    {
        rs->needs_redraw = false;
        return 0;
    }

    rs->dbg_tile_us = rs->dbg_gpu_us = rs->dbg_overlay_us = 0;

    if (!rs->live_track_mode && rs->gpu_ok && rs->base_tile)
    {
        if (!rs->base_tile_valid)
            build_base_tile(rs);

        /* one GPU transform per frame: rotate+scale the static base.
         * (Always the polling HAL_EPIC_Rotate - the interrupt-mode copy
         * would need the EPIC IRQ handler attached and hangs otherwise.) */
        uint32_t t2 = get_time_us();
        gpu_fill_rect(rs->cbuf, rs->buf_width, rs->buf_height,
                      0, 0, rs->buf_width - 1, rs->buf_height - 1,
                      PIXEL_NAVY);

        gpu_rotate_scale(rs->base_tile, rs->tile_edge, rs->tile_edge,
                         rs->cbuf, rs->buf_width, rs->buf_height,
                         rs->buf_width / 2, rs->buf_height / 2,
                         rs->viewport.rotation_deg,
                         rs->gesture_scale);
        rs->dbg_gpu_us = get_time_us() - t2;

        /* screen-space grid (scrolls with the viewport) */
        if (rs->show_grid)
            renderer_draw_grid(rs);

        /* route + markers projected onto the framebuffer (cheap CPU) */
        if (rs->show_gpx && rs->gpx && rs->gpx->loaded)
        {
            renderer_draw_route(rs);
            renderer_draw_markers(rs);
        }
    }
    else
    {
        /* Direct software path: background (GPU fill if available).
         * In live-track mode the grid + scale bar belong to the trail
         * (nice-meter cells anchored to the runner), so skip them here. */
        gpu_fill_rect(rs->cbuf, rs->buf_width, rs->buf_height,
                      0, 0, rs->buf_width - 1, rs->buf_height - 1,
                      PIXEL_NAVY);

        if (rs->show_grid && !rs->live_track_mode)
            renderer_draw_grid(rs);

        if (rs->show_gpx && rs->gpx && rs->gpx->loaded)
        {
            renderer_draw_route(rs);
            renderer_draw_markers(rs);
        }
    }

    /* Screen-space overlays (never rotated). */
    uint32_t t3 = get_time_us();
    renderer_draw_compass(rs);
    if (!rs->live_track_mode)
    {
        renderer_draw_scale_bar(rs);
        renderer_draw_info_overlay(rs);
    }
    if (rs->show_elevation_profile)
        renderer_draw_elevation_profile(rs);
    rs->dbg_overlay_us = get_time_us() - t3;

    rs->frame_count++;
    rs->render_time_us = get_time_us() - t0;
    if (rs->render_time_us > 0)
    {
        rs->current_fps = 1000000.0f / (float)rs->render_time_us;
    }
    rs->last_frame_ms = now_ms;
    rs->needs_redraw = false;
    return 1;
}

/* ------------------------------------------------------------------ *
 * draw functions
 * ------------------------------------------------------------------ */

void renderer_draw_grid(render_state_t *rs)
{
    double m_per_px = 10.0 / (double)rs->viewport.zoom;  /* px/m = zoom/10 */
    float cell_m = nice_grid_m((float)(1.0 / m_per_px));
    double cell_deg = (double)cell_m / METERS_PER_DEG;
    double lat_min, lat_max, lon_min, lon_max;
    double lat0, lon0, lat1, lon1;
    double lat, lon;
    pixel_t color = pixel_rgb(46, 46, 78);

    if (!rs->cbuf || cell_m <= 0.0f)
        return;

    /* visible geo range = min/max over the four screen corners, so the
     * grid also covers the full screen when the map is rotated */
    screen_to_geo(rs, 0, 0, &lat0, &lon0);
    screen_to_geo(rs, rs->buf_width, 0, &lat1, &lon1);
    lat_min = lat0 < lat1 ? lat0 : lat1;
    lat_max = lat0 > lat1 ? lat0 : lat1;
    lon_min = lon0 < lon1 ? lon0 : lon1;
    lon_max = lon0 > lon1 ? lon0 : lon1;
    screen_to_geo(rs, 0, rs->buf_height, &lat0, &lon0);
    screen_to_geo(rs, rs->buf_width, rs->buf_height, &lat1, &lon1);
    if (lat0 < lat_min) lat_min = lat0;
    if (lat0 > lat_max) lat_max = lat0;
    if (lat1 < lat_min) lat_min = lat1;
    if (lat1 > lat_max) lat_max = lat1;
    if (lon0 < lon_min) lon_min = lon0;
    if (lon0 > lon_max) lon_max = lon0;
    if (lon1 < lon_min) lon_min = lon1;
    if (lon1 > lon_max) lon_max = lon1;

    /* vertical lines (constant longitude), rotate with the map */
    for (lon = floor(lon_min / cell_deg) * cell_deg;
         lon <= lon_max; lon += cell_deg)
    {
        int16_t x0, y0, x1, y1;
        geo_to_screen(rs, lat_min, lon, &x0, &y0);
        geo_to_screen(rs, lat_max, lon, &x1, &y1);
        draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
                  x0, y0, x1, y1, color, 1);
    }

    /* horizontal lines (constant latitude) */
    for (lat = floor(lat_min / cell_deg) * cell_deg;
         lat <= lat_max; lat += cell_deg)
    {
        int16_t x0, y0, x1, y1;
        geo_to_screen(rs, lat, lon_min, &x0, &y0);
        geo_to_screen(rs, lat, lon_max, &x1, &y1);
        draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
                  x0, y0, x1, y1, color, 1);
    }
}

static pixel_t elevation_color(float ele, float min_ele, float max_ele)
{
    float range = max_ele - min_ele;
    float t = (range < 1.0f) ? 0.5f : (ele - min_ele) / range;
    uint8_t r, g, b;

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    if (t < 0.5f)
    {
        r = (uint8_t)(255.0f * (2.0f * t));
        g = 255;
        b = 0;
    }
    else
    {
        r = 255;
        g = (uint8_t)(255.0f * (2.0f * (1.0f - t)));
        b = 0;
    }
    return pixel_rgb(r, g, b);
}

/* ------------------------------------------------------------------ *
 * live run track (sport-watch style)
 * ------------------------------------------------------------------ */

/* nice-meter grid anchored to the runner (lines pass through the
 * current position; round cell sizes adapt to the zoom level) */
static void draw_track_grid(render_state_t *rs, float scale,
                            float cx, float cy)
{
    float cell_m = nice_grid_m(scale);
    float cell_px = cell_m * scale;
    pixel_t color = pixel_rgb(38, 40, 72);
    int k;

    if (cell_px < 6.0f)
        return;

    /* vertical lines */
    k = (int)floorf(-cx / cell_px);
    for (;; k++)
    {
        float x = cx + (float)k * cell_px;
        int16_t xi;
        if (x > (float)rs->buf_width)
            break;
        xi = (int16_t)x;
        if (xi >= 0)
            draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
                      xi, 0, xi, rs->buf_height - 1, color, 1);
    }

    /* horizontal lines */
    k = (int)floorf(-cy / cell_px);
    for (;; k++)
    {
        float y = cy + (float)k * cell_px;
        int16_t yi;
        if (y > (float)rs->buf_height)
            break;
        yi = (int16_t)y;
        if (yi >= 0)
            draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
                      0, yi, rs->buf_width - 1, yi, color, 1);
    }
}

/* scale bar for the live track: exactly one grid cell, labelled with
 * the round distance so the map reads like a real navigation app */
static void draw_track_scale_bar(render_state_t *rs, float scale)
{
    float cell_m = nice_grid_m(scale);
    float cell_px = cell_m * scale;
    int16_t x = 12;
    int16_t y = rs->buf_height - 66;   /* just above the data card */
    int16_t bar_px = (int16_t)cell_px;
    char label[16];

    if (bar_px < 18) bar_px = 18;
    if (bar_px > 220) bar_px = 220;

    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              x, y, x + bar_px, y, PIXEL_WHITE, 4);
    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              x, y - 4, x, y + 4, PIXEL_WHITE, 2);
    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              x + bar_px, y - 4, x + bar_px, y + 4, PIXEL_WHITE, 2);

    if (cell_m >= 1000.0f)
        snprintf(label, sizeof(label), "%dkm", (int)(cell_m / 1000.0f));
    else
        snprintf(label, sizeof(label), "%dm", (int)cell_m);
    ui_text(rs->cbuf, rs->buf_width, rs->buf_height,
              x + bar_px / 2 - 10, y - 18, label, PIXEL_WHITE);
}

/* draw the live dead-reckoned run track, big-tech sport watch style:
 *   - the CURRENT position is anchored near the viewport center, the
 *     track trails behind it (like a real GPS run screen)
 *   - north = up, east = right (standard map orientation)
 *   - the scale auto-fits the whole trail and shrinks smoothly as it
 *     grows (Garmin auto-zoom); a pinch switches to manual zoom
 *   - segments are colored by pace zone (Strava/Garmin heat trail),
 *     older segments fade toward the background (breadcrumb depth)
 *   - round split dots every km, a green start marker, a pulsing
 *     current-position ring with a heading tick, and a scale bar that
 *     matches the grid cell
 */
void renderer_draw_run_track(render_state_t *rs,
                             const run_track_point_t *track, int n,
                             float heading_deg, uint32_t now_ms)
{
    pixel_t *buf = rs->cbuf;
    float cur_x, cur_y, scale, cx, cy, view_r;
    float acc_km = 0.0f;
    int i, step = 1;
    int16_t px, py;

    if (!buf || n < 1)
        return;

    cur_x = track[n - 1].x;
    cur_y = track[n - 1].y;

    /* adaptive auto-fit: shrink smoothly as the trail grows; a manual
     * pinch disables it until double-tap / new run resets */
    if (rs->track_fit_auto && n >= 2)
    {
        float span = 0.0f;
        int s;
        if (n > 512) step = n / 512;
        view_r = TRACK_VIEW_FRAC *
                 (float)(rs->buf_width < rs->buf_height ?
                         rs->buf_width : rs->buf_height);
        for (s = 0; s < n; s += step)
        {
            float dx = track[s].x - cur_x;
            float dy = track[s].y - cur_y;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > span) span = d;
        }
        if (span < 0.5f) span = 0.5f;
        scale = view_r / span;
        if (scale < TRACK_SCALE_MIN) scale = TRACK_SCALE_MIN;
        if (scale > TRACK_SCALE_MAX) scale = TRACK_SCALE_MAX;
        rs->track_scale += (scale - rs->track_scale) * TRACK_FIT_SMOOTH;
    }
    else if (!rs->track_fit_auto)
    {
        rs->track_scale = rs->track_manual_scale;
    }

    scale = rs->track_scale * rs->gesture_scale;
    if (scale < TRACK_SCALE_MIN) scale = TRACK_SCALE_MIN;
    if (scale > TRACK_SCALE_MAX) scale = TRACK_SCALE_MAX;

    /* anchor: runner just below the screen center (look-ahead above) */
    cx = rs->buf_width / 2.0f + rs->viewport.offset_x;
    cy = rs->buf_height / 2.0f + rs->viewport.offset_y + 24.0f;

    /* grid with round cell sizes, anchored to the runner */
    draw_track_grid(rs, scale, cx, cy);

    if (n >= 2)
    {
        /* polyline with pace-zone coloring; old trail fades toward bg */
        if (n > TRACK_MAX_DRAW_PTS)
            step = (n + TRACK_MAX_DRAW_PTS - 1) / TRACK_MAX_DRAW_PTS;
        else
            step = 1;

        for (i = 1; i < n; i += step)
        {
            int16_t x0 = (int16_t)(cx + (track[i - 1].x - cur_x) * scale);
            int16_t y0 = (int16_t)(cy - (track[i - 1].y - cur_y) * scale);
            int16_t x1 = (int16_t)(cx + (track[i].x - cur_x) * scale);
            int16_t y1 = (int16_t)(cy - (track[i].y - cur_y) * scale);
            float seg_m = sqrtf(
                (track[i].x - track[i - 1].x) * (track[i].x - track[i - 1].x) +
                (track[i].y - track[i - 1].y) * (track[i].y - track[i - 1].y));
            pixel_t col;

            acc_km += seg_m / 1000.0f;

            /* cull segments fully outside the screen */
            if ((x0 < -40 && x1 < -40) ||
                (x0 > rs->buf_width + 40 && x1 > rs->buf_width + 40) ||
                (y0 < -40 && y1 < -40) ||
                (y0 > rs->buf_height + 40 && y1 > rs->buf_height + 40))
                continue;

            col = renderer_pace_color(track[i].pace_s_per_km);
            if (i < n - TRACK_FADE_POINTS)
                col = mix565(col, PIXEL_NAVY, TRACK_FADE_MIX);

            draw_line(buf, rs->buf_width, rs->buf_height,
                      x0, y0, x1, y1, col, 3);

            /* round split dot every km */
            if (acc_km >= 1.0f)
            {
                draw_circle(buf, rs->buf_width, rs->buf_height,
                            x1, y1, TRACK_KM_DOT_R,
                            pixel_rgb(255, 255, 255), true);
                acc_km -= 1.0f;
            }
        }

        /* start marker: green dot with white core */
        px = (int16_t)(cx + (track[0].x - cur_x) * scale);
        py = (int16_t)(cy - (track[0].y - cur_y) * scale);
        if (px > -10 && px < rs->buf_width + 10 &&
            py > -10 && py < rs->buf_height + 10)
        {
            draw_circle(buf, rs->buf_width, rs->buf_height,
                        px, py, 6, PIXEL_BLACK, true);
            draw_circle(buf, rs->buf_width, rs->buf_height,
                        px, py, 5, pixel_rgb(0, 255, 0), true);
            draw_circle(buf, rs->buf_width, rs->buf_height,
                        px, py, 3, PIXEL_WHITE, true);
        }
    }

    /* current position: pulsing ring + solid dot + heading tick */
    px = (int16_t)cx;
    py = (int16_t)cy;
    {
        float ph = (float)(now_ms % 1000) * 0.0062831853f; /* 2pi/1000 */
        float pr = 9.0f + 3.0f * sinf(ph);
        float alpha = 0.5f + 0.5f * sinf(ph);
        pixel_t ring = mix565(pixel_rgb(46, 229, 157), PIXEL_NAVY, alpha);

        draw_circle(buf, rs->buf_width, rs->buf_height,
                    px, py, (int16_t)pr, ring, false);
        draw_circle(buf, rs->buf_width, rs->buf_height,
                    px, py, 4, PIXEL_WHITE, true);
        draw_circle(buf, rs->buf_width, rs->buf_height,
                    px, py, 2, pixel_rgb(46, 229, 157), true);
    }
    {
        float rad = heading_deg * (float)M_PI / 180.0f;
        float len = 24.0f;
        int16_t hx = (int16_t)(px + sinf(rad) * len);
        int16_t hy = (int16_t)(py - cosf(rad) * len);
        draw_line(buf, rs->buf_width, rs->buf_height,
                  px, py, hx, hy, pixel_rgb(46, 229, 157), 2);
        draw_circle(buf, rs->buf_width, rs->buf_height,
                    hx, hy, 2, pixel_rgb(46, 229, 157), true);
    }

    /* scale bar matching the grid cell, above the data card */
    draw_track_scale_bar(rs, scale);
}

void renderer_draw_route(render_state_t *rs)
{
    uint16_t i;
    pixel_t *buf = rs->cbuf;
    int16_t  buf_w = rs->buf_width;
    int16_t  buf_h = rs->buf_height;
    double cumulative = 0.0;
    uint32_t dbg_proj = 0, dbg_draw = 0;
    uint32_t dbg_t0 = get_time_us();

    if (!rs->gpx || rs->gpx->point_count < 2)
        return;

    for (i = 1; i < rs->gpx->point_count; i++)
    {
        int16_t x0, y0, x1, y1;
        double d;
        uint32_t dt1 = get_time_us();

        geo_to_screen(rs, rs->gpx->points[i - 1].lat,
                      rs->gpx->points[i - 1].lon, &x0, &y0);
        geo_to_screen(rs, rs->gpx->points[i].lat,
                      rs->gpx->points[i].lon, &x1, &y1);

        /* cull segments fully outside the buffer */
        if ((x0 < -60 && x1 < -60) || (x0 > buf_w + 60 && x1 > buf_w + 60) ||
            (y0 < -60 && y1 < -60) || (y0 > buf_h + 60 && y1 > buf_h + 60))
        {
            cumulative += gpx_point_distance(
                rs->gpx->points[i - 1].lat, rs->gpx->points[i - 1].lon,
                rs->gpx->points[i].lat, rs->gpx->points[i].lon);
            continue;
        }

        d = gpx_point_distance(rs->gpx->points[i - 1].lat,
                               rs->gpx->points[i - 1].lon,
                               rs->gpx->points[i].lat,
                               rs->gpx->points[i].lon);
        cumulative += d;
        dbg_proj += get_time_us() - dt1;

        uint32_t dt2 = get_time_us();

        pixel_t color;
        if (rs->gpx->max_elevation > rs->gpx->min_elevation + 5.0f)
        {
            color = elevation_color(rs->gpx->points[i].ele,
                                    rs->gpx->min_elevation,
                                    rs->gpx->max_elevation);
        }
        else
        {
            /* hue cycles every kilometer: mileage color blocks */
            double km = cumulative / 1000.0;
            float t = (float)((km - floor(km)) * 0.5f +
                              floor(km) * 0.13f);
            if (t > 1.0f) t -= floorf(t);
            float h = t * 6.0f;
            uint8_t r, g, b;
            if (h < 1.0f)      { r = 0;   g = (uint8_t)(255 * h); b = 255; }
            else if (h < 2.0f) { r = 0;   g = 255; b = (uint8_t)(255 * (2 - h)); }
            else if (h < 3.0f) { r = (uint8_t)(255 * (h - 2)); g = 255; b = 0; }
            else if (h < 4.0f) { r = 255; g = (uint8_t)(255 * (4 - h)); b = 0; }
            else if (h < 5.0f) { r = 255; g = 0; b = (uint8_t)(255 * (h - 4)); }
            else               { r = (uint8_t)(255 * (6 - h)); g = 0; b = 255; }
            color = pixel_rgb(r, g, b);
        }

        draw_line(buf, buf_w, buf_h, x0, y0, x1, y1, color,
                  LINE_WIDTH_BASE + 1);
        dbg_draw += get_time_us() - dt2;
    }

    if (rs->frame_count < 3)
    {
        printf("[Route] proj=%uus draw=%uus total=%uus pts=%u\\n",
               (unsigned)dbg_proj, (unsigned)dbg_draw,
               (unsigned)(get_time_us() - dbg_t0),
               rs->gpx->point_count);
    }
}

void renderer_draw_markers(render_state_t *rs)
{
    uint16_t i;
    int16_t  sx, sy;

    if (!rs->gpx || rs->gpx->point_count == 0)
        return;

    /* start marker: green with black ring */
    geo_to_screen(rs, rs->gpx->points[0].lat, rs->gpx->points[0].lon,
                  &sx, &sy);
    draw_circle(rs->cbuf, rs->buf_width, rs->buf_height, sx, sy,
                MARKER_RADIUS + 2, PIXEL_BLACK, true);
    draw_circle(rs->cbuf, rs->buf_width, rs->buf_height, sx, sy,
                MARKER_RADIUS, pixel_rgb(0, 255, 0), true);

    /* end marker: red with black ring */
    i = rs->gpx->point_count - 1;
    geo_to_screen(rs, rs->gpx->points[i].lat, rs->gpx->points[i].lon,
                  &sx, &sy);
    draw_circle(rs->cbuf, rs->buf_width, rs->buf_height, sx, sy,
                MARKER_RADIUS + 2, PIXEL_BLACK, true);
    draw_circle(rs->cbuf, rs->buf_width, rs->buf_height, sx, sy,
                MARKER_RADIUS, pixel_rgb(255, 0, 0), true);

    if (rs->show_waypoints)
    {
        for (i = 0; i < rs->gpx->waypoint_count; i++)
        {
            geo_to_screen(rs, rs->gpx->waypoints[i].lat,
                          rs->gpx->waypoints[i].lon, &sx, &sy);
            draw_circle(rs->cbuf, rs->buf_width, rs->buf_height, sx, sy, 5,
                        pixel_rgb(68, 136, 255), true);
        }
    }
}

void renderer_draw_compass(render_state_t *rs)
{
    /* top-CENTRE (the panel corners are rounded, so nothing may live in
     * the corner quadrants) */
    int16_t cx = rs->buf_width / 2;
    int16_t cy = 22;
    float angle = -rs->viewport.rotation_deg * (float)DEG_TO_RAD;
    int16_t nx, ny, sx, sy;

    nx = cx + (int16_t)(COMPASS_RADIUS * sinf(angle));
    ny = cy - (int16_t)(COMPASS_RADIUS * cosf(angle));
    sx = cx - (int16_t)(10 * sinf(angle));
    sy = cy + (int16_t)(10 * cosf(angle));

    draw_circle(rs->cbuf, rs->buf_width, rs->buf_height, cx, cy,
                COMPASS_RADIUS, PIXEL_DARKGRAY, true);
    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              cx, cy, nx, ny, PIXEL_RED, 2);
    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              cx, cy, sx, sy, PIXEL_GRAY, 2);
    ui_text(rs->cbuf, rs->buf_width, rs->buf_height,
            cx - 3, cy + COMPASS_RADIUS + 2, "N", PIXEL_WHITE);
}

void renderer_draw_scale_bar(render_state_t *rs)
{
    int16_t y = rs->buf_height - 150;  /* well clear of the rounded corners */
    float scale_px_m = rs->viewport.zoom / 10.0f;
    float cell_m = nice_grid_m(scale_px_m);
    int16_t bar_px = (int16_t)(cell_m * scale_px_m);
    int16_t x;
    char label[16];

    /* the elevation profile occupies the bottom strip: hide the bar */
    if (rs->show_elevation_profile)
        return;

    if (bar_px > 220) bar_px = 220;
    if (bar_px < 18) bar_px = 18;
    x = (int16_t)((rs->buf_width - bar_px) / 2);   /* centred on the axis */

    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              x, y, x + bar_px, y, PIXEL_WHITE, 4);
    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              x, y - 4, x, y + 4, PIXEL_WHITE, 2);
    draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
              x + bar_px, y - 4, x + bar_px, y + 4, PIXEL_WHITE, 2);

    if (cell_m >= 1000.0f)
        snprintf(label, sizeof(label), "%dkm", (int)(cell_m / 1000.0f));
    else
        snprintf(label, sizeof(label), "%dm", (int)cell_m);
    ui_text(rs->cbuf, rs->buf_width, rs->buf_height,
            rs->buf_width / 2 - (int)strlen(label) * 3, y - 18,
            label, PIXEL_WHITE);
}

void renderer_draw_info_overlay(render_state_t *rs)
{
    if (!rs->gpx || !rs->gpx->loaded)
    {
        ui_text(rs->cbuf, rs->buf_width, rs->buf_height,
                  rs->buf_width / 2 - 48, rs->buf_height / 2 - 4,
                  "NO ROUTE", PIXEL_GRAY);
    }
}

void renderer_draw_elevation_profile(render_state_t *rs)
{
    uint16_t i;
    int16_t profile_h = 72;
    int16_t profile_y = rs->buf_height - profile_h - 100; /* rounded corners */
    int16_t profile_w = rs->buf_width - 24;
    int16_t profile_x = 12;
    float ele_range;

    if (!rs->gpx || rs->gpx->point_count < 2)
        return;

    gpu_fill_rect(rs->cbuf, rs->buf_width, rs->buf_height,
                  profile_x, profile_y, profile_x + profile_w - 1,
                  profile_y + profile_h - 1, pixel_rgb(0, 0, 64));

    ele_range = rs->gpx->max_elevation - rs->gpx->min_elevation;
    if (ele_range < 1.0f) ele_range = 1.0f;

    for (i = 1; i < rs->gpx->point_count; i++)
    {
        int16_t x0 = profile_x + (int16_t)((i - 1) * profile_w /
                                           (rs->gpx->point_count - 1));
        int16_t x1 = profile_x + (int16_t)(i * profile_w /
                                           (rs->gpx->point_count - 1));
        int16_t y0 = profile_y + profile_h -
            (int16_t)((rs->gpx->points[i - 1].ele -
                       rs->gpx->min_elevation) / ele_range * profile_h);
        int16_t y1 = profile_y + profile_h -
            (int16_t)((rs->gpx->points[i].ele -
                       rs->gpx->min_elevation) / ele_range * profile_h);

        draw_line(rs->cbuf, rs->buf_width, rs->buf_height,
                  x0, y0, x1, y1, PIXEL_ORANGE, 1);
    }
}

void renderer_deinit(render_state_t *rs)
{
    if (rs->base_tile)
    {
        free(rs->base_tile);
        rs->base_tile = NULL;
    }
    if (rs->gpu_ok)
    {
        gpu_deinit();
        rs->gpu_ok = false;
    }
}
