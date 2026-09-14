/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * touch_handler.h - multi-touch gesture engine on top of the NuttX
 * touchscreen char device (/dev/input0, FT6146).
 *
 * The FT6146 lower-half driver has been upgraded to report up to
 * TOUCH_DRV_MAX_POINTS contacts; this module translates raw touch samples
 * into the app-level gesture events: pan, pinch-zoom, double-tap,
 * long-press and swipe.
 */

#ifndef __TOUCH_HANDLER_H
#define __TOUCH_HANDLER_H

#include <stdint.h>
#include <stdbool.h>

#define TOUCH_MAX_POINTS      2     /* gestures need at most two contacts */
#define TOUCH_DRV_MAX_POINTS  5     /* contacts reported by the FT6146 */
#define TOUCH_TAP_TIMEOUT_MS  450   /* max gap between the two taps */
#define TOUCH_DOUBLETAP_RADIUS 70   /* max movement between taps (px) */
#define TOUCH_LONGPRESS_MS    800
#define TOUCH_SWIPE_THRESHOLD 30
#define TOUCH_JITTER_PX        6   /* held-still coordinate noise */
#define TOUCH_PAN_DEADZONE    3     /* px before pan starts */
#define TOUCH_PINCH_DEADZONE  12.0f /* px before pinch update fires */

typedef enum {
    TOUCH_EV_NONE = 0,
    TOUCH_EV_DOWN,
    TOUCH_EV_UP,
    TOUCH_EV_TAP,
    TOUCH_EV_DOUBLE_TAP,
    TOUCH_EV_LONG_PRESS,
    TOUCH_EV_SWIPE_LEFT,
    TOUCH_EV_SWIPE_RIGHT,
    TOUCH_EV_SWIPE_UP,
    TOUCH_EV_SWIPE_DOWN,
    TOUCH_EV_PINCH_BEGIN,
    TOUCH_EV_PINCH_UPDATE,
    TOUCH_EV_PINCH_END,
    TOUCH_EV_PAN_BEGIN,
    TOUCH_EV_PAN_UPDATE,
    TOUCH_EV_PAN_END,
} touch_event_t;

typedef struct {
    int16_t  x;               /* screen coordinates (scaled) */
    int16_t  y;
    uint16_t pressure;
    bool     active;
    uint32_t down_time_ms;
} touch_point_t;

typedef struct {
    touch_point_t points[TOUCH_MAX_POINTS];
    uint8_t       point_count;

    /* driver contact id -> gesture slot mapping (0xFF = unassigned) */
    uint8_t       id_map[TOUCH_DRV_MAX_POINTS];

    float         pinch_distance;
    float         pinch_start_distance;
    float         pinch_angle;
    float         pinch_start_angle;

    int16_t       pan_start_x;
    int16_t       pan_start_y;
    touch_event_t last_event;

    uint32_t      tap_time_ms;
    int16_t       tap_x, tap_y;
    uint8_t       tap_count;
    bool          long_press_fired;
    bool          tap_moved;

    /* raw->screen calibration learned at runtime */
    int16_t       raw_max_x;
    int16_t       raw_max_y;
    float         scale_x;
    float         scale_y;

    /* geometry of the target surface (from framebuffer) */
    int16_t       surf_w;
    int16_t       surf_h;
} touch_state_t;

typedef void (*touch_callback_t)(touch_event_t event, void *user_data);

int  touch_init(const char *dev_path);
void touch_set_callback(touch_callback_t cb, void *user_data);
void touch_state_reset(touch_state_t *state);
void touch_poll(touch_state_t *state);
uint32_t touch_get_stats(uint32_t *downs);
uint32_t touch_get_flagstats(uint32_t *moves, uint32_t *ups);
uint32_t touch_get_npoints(void);
void touch_get_last_pos(int *x, int *y);
void touch_deinit(void);

#endif /* __TOUCH_HANDLER_H */
