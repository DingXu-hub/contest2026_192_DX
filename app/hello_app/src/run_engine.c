/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * run_engine.c - see run_engine.h.
 *
 * All run metrics come from the real IMU: steps from accelerometer
 * magnitude peaks, bearing from gyroscope Z integration, distance from
 * step count x stride length.  The live track is dead-reckoned from
 * those and recorded every second (linear buffer with lossy
 * compression when it fills, so a long run still shows the full path).
 */

#include "run_engine.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* step detection thresholds (accel magnitude in m/s^2) */
#define STEP_PEAK_THRESHOLD   1.6f   /* 1.6 m/s^2 deviation from 1g */
#define STEP_MIN_INTERVAL_MS  250    /* max ~240 spm */
#define STEP_MAX_INTERVAL_MS  1200   /* min ~50 spm */
#define PACE_WINDOW_MS        10000  /* pace averaged over 10 s */

#ifdef CONFIG_ARCH_SIM
/* host simulator: synthesize a steady 160 spm run */
#define SIM_STEP_INTERVAL_MS  375
#endif

void run_init(run_engine_t *re)
{
    memset(re, 0, sizeof(*re));
    re->state = RUN_IDLE;
    re->step_length_m = RUN_DEFAULT_STRIDE_M;
    re->accel_prev = 9.81f;
}

void run_start(run_engine_t *re)
{
    memset(re, 0, sizeof(*re));
    re->state = RUN_RUNNING;
    re->step_length_m = RUN_DEFAULT_STRIDE_M;
    re->accel_prev = 9.81f;
    re->start_uptime_ms = 0; /* set by first tick */
}

void run_pause(run_engine_t *re)
{
    if (re->state == RUN_RUNNING)
        re->state = RUN_PAUSED;
}

void run_resume(run_engine_t *re)
{
    if (re->state == RUN_PAUSED)
    {
        re->state = RUN_RUNNING;
        re->last_tick_ms = 0;
    }
}

void run_stop(run_engine_t *re)
{
    if (re->state == RUN_RUNNING || re->state == RUN_PAUSED)
        re->state = RUN_FINISHED;
}

/* dead-reckon one stride along the current bearing */
static void advance_position(run_engine_t *re)
{
    float rad = re->heading_deg * (float)M_PI / 180.0f;
    re->pos_x += re->step_length_m * sinf(rad);
    re->pos_y += re->step_length_m * cosf(rad);
}

/* lossy compression: keep every other point, halves the buffer */
static void compress_track(run_engine_t *re)
{
    int j = 0;
    int i;
    for (i = 0; i < re->track_count; i += 2)
        re->track[j++] = re->track[i];
    re->track_count = j;
}

static void add_track_point(run_engine_t *re, uint32_t now_ms)
{
    float pace = re->pace_s_per_km > 0.0f ? re->pace_s_per_km
                                          : re->avg_pace_s_per_km;

    if (re->track_count >= RUN_TRACK_MAX)
        compress_track(re);
    re->track[re->track_count].x = re->pos_x;
    re->track[re->track_count].y = re->pos_y;
    re->track[re->track_count].pace_s_per_km = pace;
    re->track[re->track_count].heading_deg = re->heading_deg;
    re->track_count++;
    re->track_last_ms = now_ms;
}

/* stride length adapts to cadence (faster -> longer stride), like a
 * basic GPS-free distance model: 100 spm ~ 0.55 m ... 240 spm ~ 0.90 m */
static void update_stride(run_engine_t *re)
{
    float spm = re->cadence_spm;
    float stride;
    if (spm < 100.0f)
        stride = 0.55f;
    else if (spm > 240.0f)
        stride = 0.90f;
    else
        stride = 0.55f + (spm - 100.0f) / 140.0f * 0.35f;
    re->step_length_m = stride;
}

/* speed-peak suppression: clamp instant pace changes so a jittery step
 * interval does not make the displayed pace jump around (RunPulse style) */
#define PACE_MAX_CHANGE_RATIO 0.35f
static void smooth_pace(run_engine_t *re, float new_spk)
{
    if (new_spk <= 0.0f)
        return;
    if (re->pace_s_per_km <= 0.0f)
    {
        re->pace_s_per_km = new_spk;
        return;
    }
    float ratio = new_spk / re->pace_s_per_km;
    if (ratio > 1.0f + PACE_MAX_CHANGE_RATIO)
        new_spk = re->pace_s_per_km * (1.0f + PACE_MAX_CHANGE_RATIO);
    else if (ratio < 1.0f - PACE_MAX_CHANGE_RATIO)
        new_spk = re->pace_s_per_km * (1.0f - PACE_MAX_CHANGE_RATIO);
    re->pace_s_per_km = re->pace_s_per_km * 0.6f + new_spk * 0.4f;
}

static void add_step(run_engine_t *re, uint32_t now_ms)
{
    uint32_t dt = now_ms - re->last_step_ms;

    if (re->last_step_ms != 0 && dt < STEP_MIN_INTERVAL_MS)
        return;

    re->steps++;
    re->distance_m += re->step_length_m;
    advance_position(re);

    if (re->pace_window_start_ms == 0)
        re->pace_window_start_ms = now_ms;
    re->pace_window_steps++;

    if (now_ms - re->pace_window_start_ms >= PACE_WINDOW_MS)
    {
        float spm = (float)re->pace_window_steps * 60000.0f /
                    (float)(now_ms - re->pace_window_start_ms);
        re->cadence_spm = spm;
        update_stride(re);
        float v = spm / 60.0f * re->step_length_m;  /* m/s */
        smooth_pace(re, v > 0.05f ? 1000.0f / v : 0.0f);
        re->pace_window_steps = 0;
        re->pace_window_start_ms = now_ms;
    }

    re->last_step_ms = now_ms;
}

void run_tick(run_engine_t *re, uint32_t now_ms, float accel_mag,
              float gyro_z)
{
    uint32_t dt;

    if (re->state == RUN_IDLE || re->state == RUN_FINISHED)
        return;

    if (re->state == RUN_PAUSED)
    {
        re->last_tick_ms = now_ms;
        return;
    }

    if (re->start_uptime_ms == 0)
        re->start_uptime_ms = now_ms;

    if (re->last_tick_ms == 0)
        re->last_tick_ms = now_ms;

    dt = now_ms - re->last_tick_ms;
    if (dt > 100)
        dt = 100;                 /* clamp big gaps */
    re->running_ms += dt;
    re->last_tick_ms = now_ms;

    /* integrate the real gyroscope Z axis into the bearing.  Only while
     * actually stepping: when the wrist is still the gyro drifts, and
     * fusing a moving wrist rotation into a fixed bearing is wrong. */
    /* integrate the gravity-projected yaw rate into the bearing, but
     * only when actually turning: accumulate the rate and apply it once
     * it exceeds a deadband, so small arm-swing wobble never nudges the
     * heading (keeps a straight line straight) */
    if (dt > 0)
    {
        /* The rate comes from the 6-axis estimator (attitude6.c): already
         * gravity-projected (so wrist roll during the arm swing does not
         * leak in), spike-filtered and zeroed while still.  It is the same
         * value the compass capsule shows, which matters because the route
         * and the bearing must agree. */
        float accum = gyro_z * (float)dt / 1000.0f;
        re->turn_accum += accum;
        /* 4 deg -> 1 deg: a real route is a sequence of gentle bearing
         * changes, and a coarse deadband turned them into a staircase (and
         * made the trail look like a straight line).  attitude6's own
         * +-0.6 deg/s deadband already suppresses the sensor noise, so this
         * only has to stop float dust from accumulating while still. */
        if (re->turn_accum > 1.0f || re->turn_accum < -1.0f)
        {
            /* attitude6 yaw is clockwise-positive in the same convention
             * as this bearing (north = 0, clockwise positive) */
            re->heading_deg += re->turn_accum;
            re->turn_accum = 0.0f;
            while (re->heading_deg >= 360.0f)
                re->heading_deg -= 360.0f;
            while (re->heading_deg < 0.0f)
                re->heading_deg += 360.0f;
        }
        else if (now_ms - re->last_step_ms > 1500 && re->steps > 0)
        {
            /* standing still: keep the pending turn instead of throwing it
             * away (a turn made while pausing is still a turn) */
            re->turn_accum *= 0.98f;
        }
    }

#ifdef CONFIG_ARCH_SIM
    /* synthetic cadence (straight line: heading stays 0) */
    if (now_ms - re->last_step_ms >= SIM_STEP_INTERVAL_MS)
        add_step(re, now_ms);
#else
    /* peak detection on |accel - g| */
    {
        float dev = fabsf(accel_mag - 9.81f);

        if (dev > STEP_PEAK_THRESHOLD)
        {
            if (dev >= re->accel_peak)
            {
                re->accel_peak = dev;
                re->peak_falling = false;
            }
            else
            {
                re->peak_falling = true;
            }
        }

        if (re->peak_falling && dev < STEP_PEAK_THRESHOLD * 0.5f)
        {
            /* a step completed */
            if (now_ms - re->last_step_ms > STEP_MIN_INTERVAL_MS)
                add_step(re, now_ms);
            re->accel_peak = 0.0f;
            re->peak_falling = false;
        }
    }
#endif

    /* record a track point once per second */
    if (now_ms - re->track_last_ms >= RUN_TRACK_DT_MS)
        add_track_point(re, now_ms);

    /* overall average pace */
    if (re->running_ms > 0 && re->distance_m > 1.0f)
    {
        float v = re->distance_m * 1000.0f / (float)re->running_ms; /* m/s */
        if (v > 0.05f)
            re->avg_pace_s_per_km = 1000.0f / v;
    }
}

int run_track_read(const run_engine_t *re, const run_track_point_t **points)
{
    *points = re->track;
    return re->track_count;
}

/* ------------------------------------------------------------------ *
 * formatting
 * ------------------------------------------------------------------ */

void run_format_elapsed(const run_engine_t *re, char *out, int outlen)
{
    uint32_t sec = re->running_ms / 1000;
    snprintf(out, outlen, "%02lu:%02lu:%02lu",
             (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60),
             (unsigned long)(sec % 60));
}

void run_format_distance(const run_engine_t *re, char *out, int outlen,
                         bool use_miles)
{
    if (use_miles)
    {
        float mi = re->distance_m / 1609.344f;
        if (mi < 100.0f)
            snprintf(out, outlen, "%.2f", mi);
        else
            snprintf(out, outlen, "%.0f", mi);
    }
    else
    {
        float km = re->distance_m / 1000.0f;
        if (km < 100.0f)
            snprintf(out, outlen, "%.2f", km);
        else
            snprintf(out, outlen, "%.0f", km);
    }
}

void run_format_pace(const run_engine_t *re, char *out, int outlen,
                     bool use_miles)
{
    float spk = re->pace_s_per_km;
    if (spk <= 0.0f)
        spk = re->avg_pace_s_per_km;
    if (spk <= 0.0f)
    {
        snprintf(out, outlen, "--'--");
        return;
    }
    if (use_miles)
        spk *= 1.609344f;
    unsigned long total = (unsigned long)(spk + 0.5f);
    snprintf(out, outlen, "%lu'%02lu", total / 60, total % 60);
}
