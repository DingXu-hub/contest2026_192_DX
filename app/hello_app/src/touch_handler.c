/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * touch_handler.c - multi-touch gesture engine (see touch_handler.h).
 *
 * Reads NuttX touch_sample_s events from /dev/input0 (FT6146 lower half,
 * now multi-touch capable) and synthesizes app-level gestures.
 */

#include <nuttx/config.h>
#include <nuttx/input/touchscreen.h>

#include "touch_handler.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>

static touch_callback_t g_callback = NULL;
static void *g_cb_userdata = NULL;
static int g_touch_fd = -1;
static uint32_t g_touch_samples = 0;   /* samples read this session */
static uint32_t g_touch_downs = 0;     /* TOUCH_DOWN events */
static uint32_t g_touch_moves = 0;     /* TOUCH_MOVE events */
static uint32_t g_touch_ups = 0;       /* TOUCH_UP events */
static uint32_t g_last_flags = 0;      /* last processed point flags */
static uint32_t g_last_npoints = 0;
static int16_t g_last_x = 195;         /* last normalized touch pos */
static int16_t g_last_y = 225;

uint32_t touch_get_stats(uint32_t *downs)
{
    if (downs) *downs = g_touch_downs;
    return g_touch_samples;
}

uint32_t touch_get_flagstats(uint32_t *moves, uint32_t *ups)
{
    if (moves) *moves = g_touch_moves;
    if (ups)   *ups   = g_touch_ups;
    return g_last_flags;
}

uint32_t touch_get_npoints(void)
{
    return g_last_npoints;
}

/* last normalized touch position (for on-screen calibration display) */
void touch_get_last_pos(int *x, int *y)
{
    if (x) *x = g_last_x;
    if (y) *y = g_last_y;
}

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

int touch_init(const char *dev_path)
{
    g_touch_fd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (g_touch_fd < 0)
    {
        printf("[Touch] Failed to open %s\n", dev_path);
    }
    else
    {
        printf("[Touch] Opened %s\n", dev_path);
    }
    return g_touch_fd;
}

void touch_set_callback(touch_callback_t cb, void *user_data)
{
    g_callback = cb;
    g_cb_userdata = user_data;
}

void touch_state_reset(touch_state_t *state)
{
    memset(state, 0, sizeof(*state));
    memset(state->id_map, 0xFF, sizeof(state->id_map));
    state->scale_x = 0.0f;
    state->scale_y = 0.0f;
}

/* map a driver contact id to a gesture slot; assign a free slot on down */
static int slot_for_id(touch_state_t *state, uint8_t id, bool assign)
{
    int s;

    if (id >= TOUCH_DRV_MAX_POINTS)
        return -1;
    if (state->id_map[id] != 0xFF)
        return state->id_map[id];
    if (!assign)
        return -1;

    for (s = 0; s < TOUCH_MAX_POINTS; s++)
    {
        if (!state->points[s].active)
        {
            state->id_map[id] = (uint8_t)s;
            return s;
        }
    }
    return -1;
}

static float point_distance(const touch_point_t *a, const touch_point_t *b)
{
    float dx = (float)a->x - b->x;
    float dy = (float)a->y - b->y;
    return sqrtf(dx * dx + dy * dy);
}

static float point_angle(const touch_point_t *a, const touch_point_t *b)
{
    return atan2f((float)b->y - a->y, (float)b->x - a->x) * 57.29578f;
}

static void normalize_point(touch_state_t *state, int16_t *x, int16_t *y)
{
    /* Learn the raw coordinate range at runtime: some FT6146 firmware
     * reports panel-native coordinates, others report 12-bit raw. */
    if (*x > state->raw_max_x) state->raw_max_x = *x;
    if (*y > state->raw_max_y) state->raw_max_y = *y;

    if (state->raw_max_x > state->surf_w && state->scale_x == 0.0f)
        state->scale_x = (float)state->surf_w / (state->raw_max_x + 1);
    if (state->raw_max_y > state->surf_h && state->scale_y == 0.0f)
        state->scale_y = (float)state->surf_h / (state->raw_max_y + 1);

    if (state->scale_x > 0.0f)
        *x = (int16_t)(*x * state->scale_x);
    if (state->scale_y > 0.0f)
        *y = (int16_t)(*y * state->scale_y);

    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= state->surf_w) *x = state->surf_w - 1;
    if (*y >= state->surf_h) *y = state->surf_h - 1;
}

static void fire(touch_event_t ev)
{
    if (g_callback)
        g_callback(ev, g_cb_userdata);
}

static void process_sample(touch_state_t *state, struct touch_sample_s *sample)
{
    int i;
    int npoints = sample->npoints;

    if (npoints < 0 || npoints > TOUCH_DRV_MAX_POINTS)
        npoints = TOUCH_DRV_MAX_POINTS;

    uint32_t now = get_time_ms();

    for (i = 0; i < npoints; i++)
    {
        struct touch_point_s *tp = &sample->point[i];
        uint8_t id = tp->id;
        int16_t x = tp->x;
        int16_t y = tp->y;
        touch_point_t *pt;

        normalize_point(state, &x, &y);

        g_last_flags = tp->flags;
        g_last_npoints = (uint32_t)npoints;
        g_last_x = x;
        g_last_y = y;

        if (tp->flags & TOUCH_DOWN)
        {
            int slot = slot_for_id(state, id, true);
            if (slot < 0)
                continue;
            pt = &state->points[slot];

            if (pt->active)
            {
                /* repeated DOWN for an active contact: treat as MOVE so
                 * gestures stay correct even if the panel driver keeps
                 * reporting DOWN while held */
                g_touch_moves++;
                pt->x = x;
                pt->y = y;
                pt->pressure = tp->pressure;
                if (state->point_count == 1)
                {
                    int16_t dx = x - state->pan_start_x;
                    int16_t dy = y - state->pan_start_y;
                    if (abs(dx) > TOUCH_PAN_DEADZONE ||
                        abs(dy) > TOUCH_PAN_DEADZONE)
                    {
                        fire(TOUCH_EV_PAN_BEGIN);
                        fire(TOUCH_EV_PAN_UPDATE);
                        state->pan_start_x = x;
                        state->pan_start_y = y;
                    }
                }
                continue;
            }

            pt->x = x;
            pt->y = y;
            pt->pressure = tp->pressure;
            pt->active = true;
            pt->down_time_ms = now;
            state->point_count++;

            if (state->point_count == 1)
            {
                /* first contact: tap / double-tap tracking + pan base */
                if (now - state->tap_time_ms < TOUCH_TAP_TIMEOUT_MS &&
                    abs(x - state->tap_x) < TOUCH_DOUBLETAP_RADIUS &&
                    abs(y - state->tap_y) < TOUCH_DOUBLETAP_RADIUS)
                {
                    state->tap_count++;
                    if (state->tap_count >= 2)
                    {
                        fire(TOUCH_EV_DOUBLE_TAP);
                        state->tap_count = 0;
                    }
                }
                else
                {
                    state->tap_count = 0;
                }
                state->tap_time_ms = now;
                state->tap_x = x;
                state->tap_y = y;
                state->tap_moved = false;
                state->long_press_fired = false;
                state->pan_start_x = x;
                state->pan_start_y = y;
                g_touch_downs++;
                fire(TOUCH_EV_DOWN);
            }
            else if (state->point_count == 2)
            {
                state->pinch_start_distance = point_distance(
                    &state->points[0], &state->points[1]);
                state->pinch_start_angle = point_angle(
                    &state->points[0], &state->points[1]);
                state->pinch_distance = state->pinch_start_distance;
                fire(TOUCH_EV_PINCH_BEGIN);
            }
        }
        else if (tp->flags & TOUCH_MOVE)
        {
            int slot = slot_for_id(state, id, false);
            if (slot < 0)
                continue;
            pt = &state->points[slot];
            if (!pt->active)
                continue;

            /* debounce: ignore tiny jitter while the finger is held
             * still, keeps taps precise on this panel */
            if (abs(x - pt->x) < TOUCH_JITTER_PX &&
                abs(y - pt->y) < TOUCH_JITTER_PX)
                continue;

            g_touch_moves++;
            pt->x = x;
            pt->y = y;
            pt->pressure = tp->pressure;

            if (state->point_count >= 2)
            {
                float dist = point_distance(&state->points[0],
                                            &state->points[1]);
                float delta = dist - state->pinch_start_distance;

                if (fabsf(delta) > TOUCH_PINCH_DEADZONE)
                {
                    state->pinch_distance = dist;
                    state->pinch_start_distance = dist;
                    fire(TOUCH_EV_PINCH_UPDATE);
                }
            }
            else if (state->point_count == 1)
            {
                int16_t dx = x - state->pan_start_x;
                int16_t dy = y - state->pan_start_y;

                if (abs(dx) > TOUCH_PAN_DEADZONE ||
                    abs(dy) > TOUCH_PAN_DEADZONE)
                {
                    if (abs(dx) > TOUCH_SWIPE_THRESHOLD ||
                        abs(dy) > TOUCH_SWIPE_THRESHOLD)
                        state->tap_moved = true;

                    if (!state->long_press_fired)
                        fire(TOUCH_EV_PAN_BEGIN);

                    fire(TOUCH_EV_PAN_UPDATE);
                    state->pan_start_x = x;
                    state->pan_start_y = y;
                }

                if (!state->long_press_fired &&
                    now - pt->down_time_ms > TOUCH_LONGPRESS_MS &&
                    !state->tap_moved)
                {
                    state->long_press_fired = true;
                    fire(TOUCH_EV_LONG_PRESS);
                }
            }
        }
        else if (tp->flags & TOUCH_UP)
        {
            int slot = slot_for_id(state, id, false);
            if (slot < 0)
                continue;
            pt = &state->points[slot];
            if (!pt->active)
                continue;

            g_touch_ups++;
            pt->active = false;
            state->point_count--;
            state->id_map[id] = 0xFF;

            if (state->point_count == 0)
                memset(state->id_map, 0xFF, sizeof(state->id_map));

            if (state->point_count == 1)
            {
                fire(TOUCH_EV_PINCH_END);
            }
            else if (state->point_count == 0)
            {
                /* swipes do NOT fire page changes: on a watch the
                 * physical button navigates, swipes only pan the map.
                 * A small movement without panning counts as a tap. */
                if (!state->long_press_fired && !state->tap_moved)
                {
                    int16_t dx = x - state->tap_x;
                    int16_t dy = y - state->tap_y;

                    if (abs(dx) <= TOUCH_SWIPE_THRESHOLD &&
                        abs(dy) <= TOUCH_SWIPE_THRESHOLD)
                        fire(TOUCH_EV_TAP);
                }

                if (state->long_press_fired)
                    fire(TOUCH_EV_PAN_END);

                fire(TOUCH_EV_UP);
            }
        }
    }
}

void touch_poll(touch_state_t *state)
{
    uint8_t raw_buf[sizeof(struct touch_sample_s) +
                    (TOUCH_DRV_MAX_POINTS - 1) * sizeof(struct touch_point_s)]
        __attribute__((aligned(8)));
    ssize_t n;
    size_t off;

    if (g_touch_fd < 0)
        return;

    /* Drain queued samples, bounded: a pathological interrupt storm from
     * the touch controller (e.g. after the panel powers off) must not be
     * able to starve the render loop. */
    int processed = 0;
    for (;;)
    {
        if (processed >= 16)
            break;
        n = read(g_touch_fd, raw_buf, sizeof(raw_buf));
        if (n < (ssize_t)sizeof(struct touch_sample_s))
            break; /* EAGAIN / EOF / partial */
        processed++;
        g_touch_samples++;

        off = 0;
        while (n - (ssize_t)off >= (ssize_t)sizeof(struct touch_sample_s))
        {
            struct touch_sample_s *sample =
                (struct touch_sample_s *)(raw_buf + off);
            int npoints = sample->npoints;

            if (npoints < 0 || npoints > TOUCH_DRV_MAX_POINTS)
                npoints = TOUCH_DRV_MAX_POINTS;

            size_t sample_len = SIZEOF_TOUCH_SAMPLE_S(npoints);
            if (n - (ssize_t)off < (ssize_t)sample_len)
                break; /* truncated tail; ignore */

            process_sample(state, sample);
            off += sample_len;
        }
    }
}

void touch_deinit(void)
{
    if (g_touch_fd >= 0)
    {
        close(g_touch_fd);
        g_touch_fd = -1;
    }
}
