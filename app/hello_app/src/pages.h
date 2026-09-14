/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pages.h - page layer: render the current page and route touch
 * gestures to page actions.  The route preview page delegates to the
 * existing route_renderer.
 */

#ifndef __PAGES_H
#define __PAGES_H

#include <stdint.h>
#include <stdbool.h>

#include "ui.h"
#include "run_engine.h"
#include "route_renderer.h"
#include "sensor_manager.h"
#include "power_manager.h"
#include "touch_handler.h"

typedef struct {
    ui_state_t       ui;
    run_engine_t     run;
    render_state_t  *renderer;
    sensor_manager_t *sensors;
    power_manager_t  *pm;
    gpx_data_t      *gpx;
    bool             use_miles;
    int              brightness_mode;   /* 0 = auto (ALS), 1..100 = manual */
    int32_t          today_steps;
    bool             run_saved;         /* last run persisted */
    /* magnetometer calibration progress (overlay shows while running) */
    uint32_t         calib_end_ms;
    uint32_t         calib_dur_ms;
    uint32_t         now_ms;          /* last tick timestamp */
    bool             key_activate;    /* KEY2-long acts as a coordinate-less tap */
    int16_t          decl_deg;        /* magnetic -> true north offset (-20..+20) */
} app_ctx_t;

void app_ctx_init(app_ctx_t *ctx, render_state_t *r, sensor_manager_t *s,
                  power_manager_t *p, gpx_data_t *g);
void page_set(app_ctx_t *ctx, page_id_t page);
void page_next(app_ctx_t *ctx);
void page_prev(app_ctx_t *ctx);

/* render the current page into renderer->cbuf (full-screen);
 * returns 1 if anything was drawn */
int page_render(app_ctx_t *ctx, uint32_t now_ms);

/* route touch events to the current page */
void page_handle_touch(app_ctx_t *ctx, touch_event_t ev);

/* called every frame with fresh sensor data */
void page_tick(app_ctx_t *ctx, uint32_t now_ms);

#endif /* __PAGES_H */
