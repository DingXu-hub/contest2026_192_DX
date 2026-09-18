/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * attitude6.c - 6-axis attitude, built on the official Fusion library.
 *
 * WHY THIS LAYOUT (following upstream practice)
 * --------------------------------------------
 * xioTechnologies/Fusion is the reference implementation for this problem,
 * and its own answer to "attitude without a magnetometer" is:
 *
 *     gyro --> FusionBiasUpdate()            (runtime gyro offset, stillness-
 *                                             gated low-pass, per-axis
 *                                             threshold + stationary period)
 *          --> FusionAhrsUpdateNoMagnetometer()  (quaternion + half-gravity
 *                                             inclination feedback, angle-based
 *                                             acceleration rejection with
 *                                             hysteresis/recovery, startup gain
 *                                             ramp, gyroscope overrange)
 *          --> FusionQuaternionToEuler()      (roll/pitch/yaw)
 *
 * This file is a thin wrapper around exactly that chain, plus one documented
 * deviation: the official configuration cannot catch the 18-30 deg/s
 * single-sample read glitches this board produces (its overrange check only
 * trips near the configured +-2000 deg/s full scale, and it assumes a clean
 * gyro signal), so the raw sample passes a spike filter first.
 *
 * Everything else - the sign conventions, the units, and the question "does
 * this actually track reality" - is verified by the synthetic self-test at
 * the bottom of this file (run on the device with `!att test`): the inputs
 * are generated from a KNOWN attitude and known rates, and the values that
 * come back out of the official library are compared against them.
 */

#include <nuttx/config.h>

#include "attitude6.h"

#include "fusion/Fusion.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#define DEG (57.2957795f)
#define RAD (0.0174532925f)

/* LSM6DS3 full scale used by the driver (+-2000 deg/s) */
#define GYRO_RANGE_DPS   2000.0f
/* consecutive over-threshold samples before the input is treated as real
 * motion rather than a read glitch (150 ms at the 20 Hz loop) */
#define SPIKE_RUN_MAX    3
/* official-style settings; 20 Hz nominal loop (the real dt is fed per sample) */
#define NOMINAL_RATE_HZ  20.0f

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
    FusionAhrsSettings settings;

    memset(a, 0, sizeof(*a));

    /* ---------- official chain ---------- */
    FusionBiasInitialise(&a->bias);
    {
        FusionBiasSettings bs = fusionBiasDefaultSettings;

        bs.sampleRate = NOMINAL_RATE_HZ;    /* 3 deg/s threshold, 3 s still */
        FusionBiasSetSettings(&a->bias, &bs);
    }

    FusionAhrsInitialise(&a->ahrs);
    settings = fusionAhrsDefaultSettings;
    settings.sampleRate = NOMINAL_RATE_HZ;
    settings.convention = FusionConventionNed;
    settings.gyroscopeRange = GYRO_RANGE_DPS;
    settings.gain = 0.5f;
    /* official defaults: no rejection unless configured; 15 deg is the value
     * used by most Fusion integrations for wrist/vehicle motion */
    settings.accelerationRejection = 15.0f;
    settings.rejectionTimeout = 5.0f;
    FusionAhrsSetSettings(&a->ahrs, &settings);

    /* ---------- input conditioning (documented deviation) ---------- */
    a->spike_enabled = true;
    a->spike_ratio = 4.0f;
    a->spike_floor = 3.0f;      /* deg/s added to the ratio test */
}

void attitude6_set_bias(attitude6_t *a, float bx, float by, float bz)
{
    FusionVector v;

    v.axis.x = bx;
    v.axis.y = by;
    v.axis.z = bz;
    FusionBiasSetOffset(&a->bias, v);
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
        if (fabsf(gx[i]) < 3.0f && fabsf(gy[i]) < 3.0f && fabsf(gz[i]) < 3.0f)
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
    FusionAhrsSetHeading(&a->ahrs, 0.0f);      /* official yaw reset */
    a->yaw = 0.0f;
}

void attitude6_update(attitude6_t *a, float ax, float ay, float az,
                      float gx, float gy, float gz, float dt)
{
    FusionVector gyroscope, accelerometer;
    FusionVector corrected;
    FusionEuler euler;
    float amag, gmag;

    if (dt <= 0.0f || dt > 0.5f)
        dt = 1.0f / NOMINAL_RATE_HZ;
    a->last_dt = dt;

    /* ---- input conditioning: reject single-sample read glitches -------- *
     * Measured on this board: occasional 18-30 deg/s spikes on a still
     * wrist.  The official library has no equivalent (its overrange check
     * trips only near +-2000 deg/s), and such a spike steps the integrated
     * heading by ~1 deg.  A genuine turn is sustained, so the running
     * average follows it and it is never rejected. */
    gmag = sqrtf(gx * gx + gy * gy + gz * gz);
    if (a->spike_enabled)
    {
        if (a->samples == 0)
        {
            a->gyro_mag_avg = gmag;            /* prime exactly once */
        }
        else if (gmag > a->spike_ratio * a->gyro_mag_avg + a->spike_floor)
        {
            /* Hysteresis, in the spirit of the library's recovery counters:
             * an isolated excursion on a still wrist is a read glitch, but a
             * sustained level is real motion - after SPIKE_RUN_MAX consecutive
             * samples the average is re-primed so the turn is followed.  Without
             * this a genuine turn out of stillness was swallowed forever (caught
             * by the `yaw ... dps` self-test cases). */
            if (++a->spike_run >= SPIKE_RUN_MAX)
            {
                a->gyro_mag_avg = gmag;
                a->spike_run = 0;
            }
            else
            {
                gx = gy = gz = 0.0f;
                a->spikes++;
            }
        }
        else
        {
            a->spike_run = 0;
            a->gyro_mag_avg += (gmag - a->gyro_mag_avg) *
                               (1.0f - expf(-dt / 2.0f));
        }
    }

    /* ---- official gyro bias (stillness gated low-pass) ---------------- */
    gyroscope.axis.x = gx;
    gyroscope.axis.y = gy;
    gyroscope.axis.z = gz;
    corrected = FusionBiasUpdate(&a->bias, gyroscope);

    a->dbg_gyro[0] = corrected.axis.x;
    a->dbg_gyro[1] = corrected.axis.y;
    a->dbg_gyro[2] = corrected.axis.z;
    a->dbg_acc[0] = ax;
    a->dbg_acc[1] = ay;
    a->dbg_acc[2] = az;

    {
        FusionVector off = FusionBiasGetOffset(&a->bias);

        a->gyro_bias[0] = off.axis.x;
        a->gyro_bias[1] = off.axis.y;
        a->gyro_bias[2] = off.axis.z;
        a->gyro_bias_valid = (fabsf(off.axis.x) + fabsf(off.axis.y) +
                              fabsf(off.axis.z)) > 0.01f;
    }

    /* ---- official attitude update (variable sample period) ------------ */
    accelerometer.axis.x = ax;
    accelerometer.axis.y = ay;
    accelerometer.axis.z = az;

    FusionAhrsSetSamplePeriod(&a->ahrs, dt);
    FusionAhrsUpdateNoMagnetometer(&a->ahrs, corrected, accelerometer);

    euler = FusionQuaternionToEuler(FusionAhrsGetQuaternion(&a->ahrs));
    a->roll  = euler.angle.roll;
    a->pitch = euler.angle.pitch;
    a->yaw   = wrap180(euler.angle.yaw);

    /* ---- independent references kept for verification / UI ------------ */
    amag = sqrtf(ax * ax + ay * ay + az * az);
    if (amag > 0.05f)
    {
        float ux = ax / amag, uy = ay / amag, uz = az / amag;
        float ca = sqrtf(ay * ay + az * az);

        a->roll_a  = atan2f(-ay, -az) * DEG;
        a->pitch_a = atan2f(ax, ca) * DEG;

        /* gravity-projected turn rate; u is the UP direction, so clockwise
         * (which increases the NED yaw) is minus the projection */
        a->yaw_rate = -(corrected.axis.x * ux + corrected.axis.y * uy +
                        corrected.axis.z * uz);
        a->still = (fabsf(amag - 1.0f) <= 0.10f) &&
                   (gmag < fusionBiasDefaultSettings.stationaryThreshold);
    }
    else
    {
        a->still = false;
        a->yaw_rate = 0.0f;
    }

    a->converged = a->samples > (uint32_t)(2.0f * NOMINAL_RATE_HZ);
    a->samples++;
}

/* ------------------------------------------------------------------ *
 * self-test: synthetic ground truth through the official chain
 * ------------------------------------------------------------------ */

/* accelerometer reading (in g) for a known roll/pitch, using the model
 * measured on this hardware:
 *     a = (sin(pitch), -cos(pitch)sin(roll), -cos(pitch)cos(roll))
 * (flat -> (0,0,-1) g, 12 o'clock raised -> ax > 0, right side down -> ay < 0) */
static void synth_accel(float roll_deg, float pitch_deg, float *ax, float *ay,
                        float *az)
{
    float r = roll_deg * RAD, p = pitch_deg * RAD;

    *ax = sinf(p);
    *ay = -cosf(p) * sinf(r);
    *az = -cosf(p) * cosf(r);
}

static void report_line(const char *name, bool ok, float got, float want,
                        const char *what, char *report, size_t len)
{
    char line[160];

    snprintf(line, sizeof(line), "%s %-18s %s %+7.1f / %+7.1f\n",
             ok ? "[PASS]" : "[FAIL]", name, what, got, want);
    if (report && len > strlen(report) + strlen(line) + 1)
        strcat(report, line);
    printf("%s", line);
}

/* FusionAhrsUpdateNoMagnetometer() deliberately forces the heading to zero
 * during the 3 s startup ramp ("a gyro-only boot must have a defined yaw"),
 * so every case warms up first and only then measures - the product code has
 * the same behaviour, which is why the compass is only meaningful a few
 * seconds after boot. */
#define CASE_WARMUP_S 4.0f

static void run_case(const char *name, float roll_true, float pitch_true,
                     float yaw_rate_dps, float seconds, float tol_deg,
                     char *report, size_t len, int *fail)
{
    attitude6_t a;
    float dt = 0.01f;               /* 100 Hz synthetic loop */
    int steps = (int)(seconds / dt);
    int warm = (int)(CASE_WARMUP_S / dt);
    int i;
    bool ok;

    attitude6_init(&a);

    for (i = -warm; i < steps; i++)
    {
        float rate = (i < 0) ? 0.0f : yaw_rate_dps;
        float ax, ay, az, amag, gx, gy, gz;

        synth_accel(roll_true, pitch_true, &ax, &ay, &az);

        /* rotation about the VERTICAL axis: omega = -u * rate, so the
         * gravity-projected rate equals +yaw_rate_dps (clockwise positive) */
        amag = sqrtf(ax * ax + ay * ay + az * az);
        gx = -(ax / amag) * rate;
        gy = -(ay / amag) * rate;
        gz = -(az / amag) * rate;

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
    report_line(name, ok, a.roll, roll_true, "roll", report, len);
    if (!ok)
        printf("       (also pitch %+.1f/%+.1f yaw %+.1f/%+.1f | lib: startup=%d gain=%.2f ramp=%.2f dt=%.4f anchored=%d)\n",
               a.pitch, pitch_true, a.yaw, wrap180(yaw_rate_dps * seconds),
               (int)a.ahrs.startup, a.ahrs.gain, a.ahrs.startupGain,
               a.ahrs.samplePeriod, (int)a.ahrs.headingAnchored);
    if (!ok)
        printf("       (dbg: gyro_in=(%.2f,%.2f,%.2f) bias=(%.2f,%.2f,%.2f) "
               "q=(%.4f,%.4f,%.4f,%.4f) acc=(%.2f,%.2f,%.2f))\n",
               a.dbg_gyro[0], a.dbg_gyro[1], a.dbg_gyro[2],
               a.gyro_bias[0], a.gyro_bias[1], a.gyro_bias[2],
               a.ahrs.quaternion.element.w, a.ahrs.quaternion.element.x,
               a.ahrs.quaternion.element.y, a.ahrs.quaternion.element.z,
               a.dbg_acc[0], a.dbg_acc[1], a.dbg_acc[2]);
}

bool attitude6_selftest(char *report, size_t report_len)
{
    int fail = 0;

    if (report && report_len)
        report[0] = '\0';

    /* static attitudes: the official chain must converge onto the truth */
    run_case("flat", 0.0f, 0.0f, 0.0f, 8.0f, 3.0f, report, report_len, &fail);
    run_case("roll right 30", 30.0f, 0.0f, 0.0f, 8.0f, 3.0f, report, report_len, &fail);
    run_case("roll left 45", -45.0f, 0.0f, 0.0f, 8.0f, 3.0f, report, report_len, &fail);
    run_case("pitch up 40", 0.0f, 40.0f, 0.0f, 8.0f, 3.0f, report, report_len, &fail);
    run_case("roll+pitch", 20.0f, -25.0f, 0.0f, 8.0f, 4.0f, report, report_len, &fail);

    /* yaw integration (clockwise positive in NED) */
    run_case("yaw +90 @60dps", 0.0f, 0.0f, 60.0f, 1.5f, 4.0f, report, report_len, &fail);
    run_case("yaw -90 @-60dps", 0.0f, 0.0f, -60.0f, 1.5f, 4.0f, report, report_len, &fail);

    /* turning while tilted */
    run_case("tilted turn 30deg", 30.0f, 0.0f, 50.0f, 2.0f, 5.0f, report, report_len, &fail);

    /* spike rejection (the board's read glitches) - the ONLY deviation from
     * the official chain, so it gets its own ground-truth case */
    {
        attitude6_t a;
        float dt = 0.01f;
        int i;
        bool ok;
        char line[170];

        attitude6_init(&a);
        for (i = 0; i < 6000; i++)            /* 60 s at 100 Hz */
        {
            float ax, ay, az, spike = 0.0f;

            synth_accel(0.0f, 0.0f, &ax, &ay, &az);
            if ((i % 200) == 0)               /* 30 deg/s spike every 2 s */
                spike = 30.0f;
            attitude6_update(&a, ax, ay, az, 0.0f, 0.0f, spike, dt);
        }
        ok = fabsf(a.yaw) < 1.5f;
        if (!ok)
            fail++;
        snprintf(line, sizeof(line),
                 "%s %-18s 30 dps spike every 2 s -> yaw %+.2f deg, %lu rejected\n",
                 ok ? "[PASS]" : "[FAIL]", "spike rejection", a.yaw,
                 (unsigned long)a.spikes);
        if (report && report_len > strlen(report) + strlen(line) + 1)
            strcat(report, line);
        printf("%s", line);
    }

    printf("[INFO] attitude6 self-test: %s (%d case(s) failed)\n",
           fail == 0 ? "ALL PASS" : "FAILURES", fail);
    return fail == 0;
}
