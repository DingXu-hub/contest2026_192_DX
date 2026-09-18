/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * attitude6.h - 6-axis (gyroscope + accelerometer) attitude estimator.
 *
 * The attitude math is the official xioTechnologies/Fusion chain - see
 * attitude6.c for why - wrapped so the rest of the firmware keeps one small
 * API.  Roll/pitch come from gravity through the library's quaternion
 * inclination feedback; yaw is a RELATIVE bearing (no magnetometer), which
 * is what a wrist device can honestly provide.
 *
 * CONVENTIONS (pinned to measured hardware behaviour)
 * --------------------------------------------------
 * Body frame: X = 12 o'clock, Y = 3 o'clock, Z = into the screen.
 * Earth frame: North-East-Down; yaw increases clockwise.
 * Inputs: accelerometer in **g** (flat reads (0,0,-1) g, 12 raised gives
 * ax > 0, right side down gives ay < 0) and gyroscope in **deg/s**.
 * The synthetic self-test (`!att test`) verifies every one of these.
 */

#ifndef __ATTITUDE6_H
#define __ATTITUDE6_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "fusion/Fusion.h"

typedef struct {
    /* official chain */
    FusionBias bias;              /* still-gated gyro offset              */
    FusionAhrs ahrs;              /* quaternion attitude, no magnetometer */

    /* input conditioning (the one documented deviation) */
    bool     spike_enabled;
    float    spike_ratio;         /* reject |gyro| > ratio*avg + floor    */
    float    spike_floor;         /* deg/s                                */
    float    gyro_mag_avg;
    uint32_t spikes;
    uint8_t  spike_run;           /* consecutive over-threshold samples    */

    /* outputs */
    float roll, pitch, yaw;       /* deg (yaw relative, -180..180)        */
    float roll_a, pitch_a;        /* accel-only reference for checks      */
    float yaw_rate;               /* gravity-projected rate, deg/s        */
    float gyro_bias[3];           /* official offset estimate, deg/s      */
    float dbg_gyro[3];            /* last corrected gyro (diagnostics)    */
    float dbg_acc[3];             /* last accelerometer (diagnostics)     */
    bool  gyro_bias_valid;
    bool  converged;
    bool  still;
    uint32_t samples;
    float    last_dt;
} attitude6_t;

void  attitude6_init(attitude6_t *a);

/* feed one sample; accel in g, gyro in deg/s, dt in seconds */
void  attitude6_update(attitude6_t *a, float ax, float ay, float az,
                       float gx, float gy, float gz, float dt);

/* make the current heading the new zero (relative-bearing use case) */
void  attitude6_zero_yaw(attitude6_t *a);

/* pre-load a gyro offset (e.g. from a boot-time stillness calibration) */
void  attitude6_set_bias(attitude6_t *a, float bx, float by, float bz);

/* estimate the gyro bias from a buffer of still samples (deg/s) */
bool  attitude6_estimate_bias(const float *gx, const float *gy, const float *gz,
                              int n, float *bx, float *by, float *bz);

/* synthetic ground-truth test through the official chain; writes a short
 * report and returns true when every case passes.  Run on the device with
 * `!att test`. */
bool  attitude6_selftest(char *report, size_t report_len);

#endif /* __ATTITUDE6_H */
