/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * attitude6.h - 6-axis (gyroscope + accelerometer) attitude estimator.
 *
 * WHY THIS EXISTS
 * ---------------
 * The magnetometer path could not be validated on this hardware (hard-iron
 * errors up to 27 uT, a 184 uT desk anomaly, and a 90 deg magnetic error in
 * the fusion).  A 6-axis estimator gives roll/pitch that are correct by
 * construction (gravity is an absolute reference) plus a *relative* yaw
 * integrated from the gyroscope - which is what a wrist device can honestly
 * provide without a trustworthy compass.  This module replaces the 9-axis
 * fusion in the product path.
 *
 * CONVENTIONS (derived from measured hardware behaviour - see the report)
 * ---------------------------------------------------------------------
 * Body frame (the watch frame):
 *     X = 12 o'clock (forward), Y = 3 o'clock (right), Z = into the screen
 * Earth frame: North-East-Down (NED).  yaw = clockwise from the (relative)
 * zero direction, i.e. turning the watch clockwise increases yaw.
 * Inputs:
 *     accelerometer in **g** (|a| = 1.0 at rest)  - proper acceleration
 *     gyroscope in **deg/s**, body frame
 * Measured facts used to pin the signs (do not "fix" them by reasoning):
 *     flat       -> az = -1 g          (a points UP: standard NED model)
 *     12 raised  -> ax > 0             (pitch > 0 = forward tilted up)
 *     right down -> ay < 0             (roll > 0 = right side down)
 * Hence:
 *     pitch = atan2(ax, sqrt(ay^2 + az^2))
 *     roll  = atan2(-ay, -az)
 *     yaw  += -(gx*ux + gy*uy + gz*uz) * dt      with u = a/|a| (up)
 * (the minus sign is what makes a clockwise turn increase yaw; projecting
 * onto gravity instead of raw Z rejects wrist swing around the forearm)
 *
 * SELF-TEST
 * ---------
 * attitude6_selftest() feeds synthetic samples generated from a KNOWN
 * attitude and checks the estimator recovers it.  It runs on the device
 * (`!att test`), so any sign/unit/frame mistake shows up as FAIL instead of
 * being argued about.
 */

#ifndef __ATTITUDE6_H
#define __ATTITUDE6_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    /* configuration */
    float accel_tau;       /* gravity-fix time constant (s), applied as 1-exp(-dt/tau) */
    float yaw_deadband;    /* deg/s, ignored below this (post-bias noise)     */
    float accel_tol_g;     /* |a| must be within this of 1 g to use gravity   */
    float still_rate;      /* deg/s: all axes below this counts as stillness  */
    float bias_tau;        /* s: time constant of the zero-rate bias tracker  */

    /* state */
    float roll, pitch, yaw;        /* deg; yaw is relative (-180..180) */
    float roll_a, pitch_a;         /* accel-only reference (deg), for checks */
    float yaw_rate;                /* gravity-projected rate (deg/s) */
    float gyro_bias[3];            /* deg/s, body frame, learned while still */
    bool  gyro_bias_valid;
    bool  gravity_valid;
    bool  converged;               /* roll/pitch settled after start */
    bool  still;                   /* body judged stationary this sample */
    uint32_t samples;
    float    last_dt;
} attitude6_t;

void  attitude6_init(attitude6_t *a);

/* feed one sample; accel in g, gyro in deg/s, dt in seconds */
void  attitude6_update(attitude6_t *a, float ax, float ay, float az,
                       float gx, float gy, float gz, float dt);

/* make the current heading the new zero (relative-bearing use case) */
void  attitude6_zero_yaw(attitude6_t *a);

/* re-learn the gyro bias from a short still window (done by the caller's
 * sampling loop; this only stores the result) */
void  attitude6_set_bias(attitude6_t *a, float bx, float by, float bz);

/* estimate the gyro bias from a buffer of still samples (deg/s) */
bool  attitude6_estimate_bias(const float *gx, const float *gy, const float *gz,
                              int n, float *bx, float *by, float *bz);

/* synthetic ground-truth test; writes a short report, returns true if all
 * cases pass.  Safe to call at runtime (uses a local estimator instance). */
bool  attitude6_selftest(char *report, size_t report_len);

#endif /* __ATTITUDE6_H */
