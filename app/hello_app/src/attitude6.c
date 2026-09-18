/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * attitude6.c - 6-axis attitude estimator + synthetic self-test.
 * See attitude6.h for the conventions and the measured facts behind them.
 */

#include <nuttx/config.h>

#include "attitude6.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define DEG (57.2957795f)
#define RAD (0.0174532925f)

static float wrap180(float d)
{
    while (d > 180.0f)
        d -= 360.0f;
    while (d < -180.0f)
        d += 360.0f;
    return d;
}

void attitude6_init(attitude6_t *a)
{
    memset(a, 0, sizeof(*a));
    a->accel_gain   = 0.02f;   /* ~50 Hz loop -> time constant ~1 s */
    a->yaw_deadband = 0.6f;
    a->accel_tol_g  = 0.20f;
}

void attitude6_set_bias(attitude6_t *a, float bx, float by, float bz)
{
    a->gyro_bias[0] = bx;
    a->gyro_bias[1] = by;
    a->gyro_bias[2] = bz;
    a->gyro_bias_valid = true;
}

bool attitude6_estimate_bias(const float *gx, const float *gy, const float *gz,
                             int n, float *bx, float *by, float *bz)
{
    double sx = 0, sy = 0, sz = 0;
    int i, used = 0;

    if (n <= 0)
        return false;

    for (i = 0; i < n; i++)
    {
        if (fabsf(gx[i]) < 15.0f && fabsf(gy[i]) < 15.0f &&
            fabsf(gz[i]) < 15.0f)
        {
            sx += gx[i];
            sy += gy[i];
            sz += gz[i];
            used++;
        }
    }
    if (used < 10)
        return false;

    *bx = (float)(sx / used);
    *by = (float)(sy / used);
    *bz = (float)(sz / used);
    return true;
}

void attitude6_zero_yaw(attitude6_t *a)
{
    a->yaw = 0.0f;
}

void attitude6_update(attitude6_t *a, float ax, float ay, float az,
                      float gx, float gy, float gz, float dt)
{
    float amag, rate, k;

    if (dt <= 0.0f || dt > 0.5f)
        dt = 0.02f;
    a->last_dt = dt;

    /* remove the learned bias (body frame) */
    if (a->gyro_bias_valid)
    {
        gx -= a->gyro_bias[0];
        gy -= a->gyro_bias[1];
        gz -= a->gyro_bias[2];
    }

    amag = sqrtf(ax * ax + ay * ay + az * az);

    if (amag > 0.05f)
    {
        float ux = ax / amag, uy = ay / amag, uz = az / amag;  /* UP */

        /* gravity is only a valid reference while |a| ~ 1 g */
        if (fabsf(amag - 1.0f) <= a->accel_tol_g)
        {
            float ca = sqrtf(ay * ay + az * az);

            a->pitch_a = atan2f(ax, ca) * DEG;
            a->roll_a  = atan2f(-ay, -az) * DEG;

            if (!a->gravity_valid)
            {
                /* first usable gravity sample: snap instead of ramping */
                a->roll  = a->roll_a;
                a->pitch = a->pitch_a;
            }
            a->gravity_valid = true;
        }

        /* yaw rate about the vertical: u is UP, so a clockwise turn (which
         * must increase yaw) is MINUS the projection onto u */
        rate = -(gx * ux + gy * uy + gz * uz);
        if (fabsf(rate) < a->yaw_deadband)
            rate = 0.0f;
        a->yaw_rate = rate;
    }
    else
    {
        a->yaw_rate = 0.0f;
    }

    /* complementary filter: gyro integration + slow gravity correction */
    a->roll  = wrap180(a->roll  + gx * dt);
    a->pitch = wrap180(a->pitch + gy * dt);

    if (a->gravity_valid)
    {
        k = a->accel_gain;
        a->roll  = wrap180(a->roll  + k * wrap180(a->roll_a  - a->roll));
        a->pitch = wrap180(a->pitch + k * wrap180(a->pitch_a - a->pitch));
        a->converged = true;
    }

    /* yaw: pure integration (no absolute reference in 6-axis) */
    a->yaw = wrap180(a->yaw + a->yaw_rate * dt);
    a->samples++;
}

/* ------------------------------------------------------------------ *
 * self-test: synthetic ground truth
 * ------------------------------------------------------------------ */

/* accelerometer reading (in g) for a known roll/pitch, using the model that
 * matches the measured hardware:
 *     a = (sin(pitch), -cos(pitch)sin(roll), -cos(pitch)cos(roll))
 */
static void synth_accel(float roll_deg, float pitch_deg, float *ax, float *ay,
                        float *az)
{
    float r = roll_deg * RAD, p = pitch_deg * RAD;

    *ax = sinf(p);
    *ay = -cosf(p) * sinf(r);
    *az = -cosf(p) * cosf(r);
}

static void case_report(const char *name, bool ok, const attitude6_t *a,
                        float roll_true, float pitch_true, float yaw_true,
                        char *report, size_t len)
{
    char line[160];

    snprintf(line, sizeof(line),
             "%s %-18s roll %+6.1f/%+6.1f  pitch %+6.1f/%+6.1f  yaw %+7.1f/%+7.1f\n",
             ok ? "[PASS]" : "[FAIL]", name,
             a->roll, roll_true, a->pitch, pitch_true, a->yaw, yaw_true);
    if (report && len > strlen(report) + strlen(line) + 1)
        strcat(report, line);
    printf("%s", line);
}

static void run_case(const char *name, float roll_true, float pitch_true,
                     float yaw_rate_dps, float seconds, float tol_deg,
                     char *report, size_t len, int *fail)
{
    attitude6_t a;
    float dt = 0.01f;               /* 100 Hz synthetic loop */
    int steps = (int)(seconds / dt);
    int i;
    bool ok;

    attitude6_init(&a);
    a.accel_gain = 0.10f;           /* settle quickly in the short run */

    for (i = 0; i < steps; i++)
    {
        float ax, ay, az, axn, ayn, azn, amag, gx, gy, gz;

        synth_accel(roll_true, pitch_true, &ax, &ay, &az);

        /* feed a rotation about the VERTICAL axis: omega = -u * rate, so the
         * estimator's yaw rate (which is -(g . u)) equals +yaw_rate_dps */
        amag = sqrtf(ax * ax + ay * ay + az * az);
        axn = ax / amag; ayn = ay / amag; azn = az / amag;
        gx = -axn * yaw_rate_dps;
        gy = -ayn * yaw_rate_dps;
        gz = -azn * yaw_rate_dps;

        attitude6_update(&a, ax, ay, az, gx, gy, gz, dt);
    }

    {
        float yaw_true = wrap180(yaw_rate_dps * seconds);

        ok = fabsf(wrap180(a.roll - roll_true)) <= tol_deg &&
             fabsf(wrap180(a.pitch - pitch_true)) <= tol_deg &&
             fabsf(wrap180(a.yaw - yaw_true)) <=
                 (tol_deg + fabsf(yaw_true) * 0.05f);
    }

    if (!ok)
        (*fail)++;
    case_report(name, ok, &a, roll_true, pitch_true,
                wrap180(yaw_rate_dps * seconds), report, len);
}

bool attitude6_selftest(char *report, size_t report_len)
{
    int fail = 0;

    if (report && report_len)
        report[0] = '\0';

    /* static attitudes: must converge onto the synthetic truth */
    run_case("flat", 0.0f, 0.0f, 0.0f, 6.0f, 2.0f, report, report_len, &fail);
    run_case("roll right 30", 30.0f, 0.0f, 0.0f, 6.0f, 2.0f, report, report_len, &fail);
    run_case("roll left 45", -45.0f, 0.0f, 0.0f, 6.0f, 2.0f, report, report_len, &fail);
    run_case("pitch up 40", 0.0f, 40.0f, 0.0f, 6.0f, 2.0f, report, report_len, &fail);
    run_case("roll+pitch", 20.0f, -25.0f, 0.0f, 8.0f, 3.0f, report, report_len, &fail);

    /* yaw integration (clockwise positive) */
    run_case("yaw +90 @90dps", 0.0f, 0.0f, 90.0f, 1.0f, 2.0f, report, report_len, &fail);
    run_case("yaw -180 @-90dps", 0.0f, 0.0f, -90.0f, 2.0f, 2.0f, report, report_len, &fail);

    /* turning while tilted: the gravity projection must still integrate the
     * vertical rotation correctly */
    run_case("tilted turn 30deg", 30.0f, 0.0f, 60.0f, 1.0f, 3.0f, report, report_len, &fail);
    run_case("tilted turn 45deg", 45.0f, 20.0f, 90.0f, 1.0f, 3.0f, report, report_len, &fail);

    /* documented intrinsic limitation: an unlearned 1 deg/s bias drifts */
    {
        attitude6_t a;
        float dt = 0.01f;
        int i;
        char line[170];

        attitude6_init(&a);
        for (i = 0; i < 6000; i++)
        {
            float ax, ay, az;

            synth_accel(0.0f, 0.0f, &ax, &ay, &az);
            attitude6_update(&a, ax, ay, az, 0.0f, 0.0f, 1.0f, dt);
        }
        snprintf(line, sizeof(line),
                 "[INFO] unlearned 1 deg/s bias over 60 s -> yaw drift %.0f deg "
                 "(intrinsic to 6-axis; the sampling loop learns the bias)\n",
                 a.yaw);
        if (report && report_len > strlen(report) + strlen(line) + 1)
            strcat(report, line);
        printf("%s", line);
    }

    printf("[INFO] attitude6 self-test: %s (%d case(s) failed)\n",
           fail == 0 ? "ALL PASS" : "FAILURES", fail);
    return fail == 0;
}
