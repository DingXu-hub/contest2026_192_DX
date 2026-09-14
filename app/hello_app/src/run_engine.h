/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * run_engine.h - run session state machine, cadence/distance/pace
 * estimation and real-time track recording.
 *
 * The watch has no GNSS, so every number is derived from real IMU data:
 *  - steps      : accelerometer magnitude peak detection (LSM6DS3)
 *  - heading    : gyroscope Z-axis integration (relative bearing)
 *  - distance   : steps x stride length (0.75 m default, user adjustable)
 *  - track      : dead-reckoned positions recorded every second while
 *                 running (x = east meters, y = north meters), each
 *                 carrying the instant pace + bearing so the renderer
 *                 can draw a pace heat map
 */

#ifndef __RUN_ENGINE_H
#define __RUN_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#define RUN_DEFAULT_STRIDE_M   0.75f

/* live track ring buffer (1 point per second -> ~34 min at 2048) */
#define RUN_TRACK_MAX         2048
#define RUN_TRACK_DT_MS       1000

typedef enum {
    RUN_IDLE = 0,
    RUN_RUNNING,
    RUN_PAUSED,
    RUN_FINISHED,
} run_state_t;

typedef struct {
    float x;                  /* east meters (dead reckoning) */
    float y;                  /* north meters */
    float pace_s_per_km;      /* instant pace at this point (0 = unknown) */
    float heading_deg;        /* bearing at this point, north = 0 */
} run_track_point_t;

typedef struct {
    run_state_t state;
    uint32_t    start_uptime_ms;   /* tick when run started */
    uint32_t    running_ms;        /* accumulated moving time */
    uint32_t    last_tick_ms;
    uint32_t    steps;
    float       distance_m;
    float       ascent_m;          /* placeholder (no barometer) */
    float       cadence_spm;       /* current steps per minute */
    float       step_length_m;
    float       pace_s_per_km;     /* current pace, seconds per km */
    float       avg_pace_s_per_km; /* overall average pace */

    /* step detection state */
    float       accel_prev;
    float       accel_peak;
    bool        peak_falling;
    uint32_t    last_step_ms;
    uint32_t    pace_window_steps;
    uint32_t    pace_window_start_ms;

    /* dead reckoning */
    float       pos_x;             /* current east meters */
    float       pos_y;             /* current north meters */
    float       heading_deg;       /* relative bearing, north = 0 */
    float       turn_accum;        /* pending turn (deg) before apply */
    uint32_t    track_last_ms;

    /* live track ring buffer */
    run_track_point_t track[RUN_TRACK_MAX];
    int         track_head;        /* index of oldest point */
    int         track_count;
} run_engine_t;

void run_init(run_engine_t *re);
void run_start(run_engine_t *re);
void run_pause(run_engine_t *re);
void run_resume(run_engine_t *re);
void run_stop(run_engine_t *re);

/* Feed one IMU sample every ~20 ms:
 *   accel_mag - accelerometer magnitude in m/s^2 (real LSM6DS3 data)
 *   gyro_z    - gyroscope Z axis in deg/s (real data; 0 on simulator)
 * On the host simulator pass accel_mag = 9.81 (synthetic stepping). */
void run_tick(run_engine_t *re, uint32_t now_ms, float accel_mag,
              float gyro_z);

/* read the live track (ring buffer, oldest first) */
int  run_track_read(const run_engine_t *re,
                    const run_track_point_t **points);

/* format helpers */
void run_format_elapsed(const run_engine_t *re, char *out, int outlen);
void run_format_distance(const run_engine_t *re, char *out, int outlen,
                         bool use_miles);
void run_format_pace(const run_engine_t *re, char *out, int outlen,
                     bool use_miles);

#endif /* __RUN_ENGINE_H */
