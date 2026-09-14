/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * route_renderer.h - vector route preview renderer for SF32LB52 / CO5300
 *
 * Two rendering modes:
 *  1. Direct CPU projection: every frame projects the GPX polyline through
 *     the current viewport (zoom/pan/rotation) and rasterizes it into the
 *     RGB565 framebuffer.  The background fill is offloaded to the
 *     ePicasso GPU when available.
 *  2. GPU tile mode (default when the EPIC engine is present): the map
 *     content (grid + route + markers) is rendered once into an unrotated
 *     square tile; every frame the tile is rotated/scaled by the ePicasso
 *     hardware around the screen center.  Compass auto-rotation then costs
 *     one GPU transform instead of a full software re-project.
 */

#ifndef __ROUTE_RENDERER_H
#define __ROUTE_RENDERER_H

#include <stdint.h>
#include <stdbool.h>

#include "gpx_parser.h"
#include "run_engine.h"   /* run_track_point_t */

/* Panel native resolution of the LCKFB Huangshan Pi 1.85" AMOLED. */
#define RENDER_WIDTH   390
#define RENDER_HEIGHT  450

/* GPU tile geometry: square tile covering any screen rotation. */
#define TILE_EDGE      (((RENDER_WIDTH + RENDER_HEIGHT) * 7) / 10 + 8)

#define ZOOM_MIN        0.5f
#define ZOOM_MAX        20.0f
#define ZOOM_DEFAULT    3.0f

/* RGB565 colors */
typedef uint16_t pixel_t;

#define PIXEL_BLACK     0x0000
#define PIXEL_WHITE     0xFFFF
#define PIXEL_RED       0xF800
#define PIXEL_GREEN     0x07E0
#define PIXEL_BLUE      0x001F
#define PIXEL_ORANGE    0xFD20
#define PIXEL_GRAY      0x8410
#define PIXEL_DARKGRAY  0x3186
#define PIXEL_NAVY      0x018C

static inline pixel_t pixel_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (pixel_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

typedef struct {
    float  zoom;
    float  offset_x;
    float  offset_y;
    float  rotation_deg;      /* 0 = north up, CCW positive */
    double center_lat;
    double center_lon;
    bool   animating;
    double anim_from_lat, anim_from_lon;
    double anim_to_lat, anim_to_lon;
    float  anim_from_zoom, anim_to_zoom;
    uint32_t anim_start_ms;
    uint32_t anim_duration_ms;
} render_viewport_t;

typedef struct {
    gpx_data_t       *gpx;
    render_viewport_t viewport;

    pixel_t          *cbuf;        /* framebuffer (mmap of /dev/fb0) */
    int16_t           buf_width;   /* actual fb width  (390) */
    int16_t           buf_height;  /* actual fb height (450) */

    /* GPU base tile: static background+grid, built once.  The route is
     * projected onto the framebuffer every frame (cheap, ~6 ms), and the
     * base tile is rotated/scaled by the ePicasso hardware. */
    pixel_t          *base_tile;
    int16_t           tile_edge;
    bool              base_tile_valid;
    bool              gpu_ok;      /* ePicasso initialized */
    float             gesture_scale; /* transient GPU zoom during pinch (1.0=off) */

    bool              needs_redraw;
    bool              show_grid;
    bool              show_elevation_profile;
    bool              show_waypoints;
    bool              show_gpx;      /* draw the loaded GPX (preview) */

    /* rotation settle detector: skip re-render when the map is stable */
    float             rotation_pending;   /* accumulated heading delta */
    uint8_t           rotation_settle_count;
    bool              rotation_settled;

    /* live run-track rendering (sport-watch style) */
    bool              live_track_mode;    /* drawing a live dead-reckoned trail */
    float             track_scale;        /* px per metre (smoothed) */
    bool              track_fit_auto;     /* auto-fit the whole trail */
    float             track_manual_scale; /* px/m when the user zoomed manually */

    uint32_t          frame_count;
    uint32_t          last_frame_ms;
    float             current_fps;
    uint32_t          last_render_us;
    uint32_t          render_time_us;
    uint32_t          dbg_tile_us;
    uint32_t          dbg_gpu_us;
    uint32_t          dbg_overlay_us;
} render_state_t;

int  renderer_init(render_state_t *rs);
void renderer_set_gpx(render_state_t *rs, gpx_data_t *gpx);
int  renderer_update(render_state_t *rs, uint32_t now_ms);   /* returns 1 if it drew */
void renderer_force_redraw(render_state_t *rs);

void renderer_set_zoom(render_state_t *rs, float zoom, int16_t cx, int16_t cy);
void renderer_pan(render_state_t *rs, int16_t dx, int16_t dy);
void renderer_set_rotation(render_state_t *rs, float deg);
void renderer_center_on_point(render_state_t *rs, double lat, double lon);
void renderer_animate_to(render_state_t *rs, double lat, double lon,
                         float zoom, uint32_t duration_ms);
void renderer_set_gesture_scale(render_state_t *rs, float scale);
void renderer_commit_gesture(render_state_t *rs);
void renderer_deinit(render_state_t *rs);

void renderer_draw_route(render_state_t *rs);
void renderer_draw_markers(render_state_t *rs);
void renderer_draw_run_track(render_state_t *rs,
                             const run_track_point_t *track, int n,
                             float heading_deg, uint32_t now_ms);
void renderer_draw_grid(render_state_t *rs);
void renderer_draw_compass(render_state_t *rs);
void renderer_draw_scale_bar(render_state_t *rs);
void renderer_draw_info_overlay(render_state_t *rs);
void renderer_draw_elevation_profile(render_state_t *rs);

/* live-track control */
void renderer_set_live_track(render_state_t *rs, bool live);
void renderer_reset_live_fit(render_state_t *rs);

/* pace-zone color for a pace in seconds/km (0 = unknown) */
pixel_t renderer_pace_color(float pace_s_per_km);

void screen_to_geo(render_state_t *rs, int16_t sx, int16_t sy,
                   double *lat, double *lon);
void geo_to_screen(render_state_t *rs, double lat, double lon,
                   int16_t *sx, int16_t *sy);

#endif /* __ROUTE_RENDERER_H */
