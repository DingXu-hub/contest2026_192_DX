/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * sensor_manager.c - see sensor_manager.h.
 *
 * The LSM6DS3TR-C registers through the in-tree NuttX lsm6dsl driver at
 * /dev/lsm6dsl0; samples are fetched with the SNIOC_LSM6DSLSENSORREAD
 * ioctl which returns struct lsm6dsl_sensor_data_s (accel in mg, gyro in
 * mdps, temperature, timestamp).  The MMC5603NJ and LTR303 are read over
 * the raw I2C char device /dev/i2c1 (the sensor bus).
 */

#include <nuttx/config.h>
#include <stdint.h>

#if defined(CONFIG_I2C) && defined(CONFIG_SENSORS_LSM6DSL)
#include <nuttx/sensors/ioctl.h>
#include <nuttx/sensors/lsm6dsl.h>
#else
/* Host simulator / builds without the LSM6DSL driver: provide a local
 * layout that matches the driver's SNIOC payload so the sensor code
 * compiles everywhere.  The ioctls are never issued when no device is
 * present. */
struct lsm6dsl_sensor_data_s
{
    int16_t  x_data;
    int16_t  y_data;
    int16_t  z_data;
    uint16_t temperature;
    int16_t  g_x_data;
    int16_t  g_y_data;
    int16_t  g_z_data;
    uint16_t timestamp;
};
#define SNIOC_START              0x4001
#define SNIOC_STOP               0x4002
#define SNIOC_LSM6DSLSENSORREAD  0x4003
#endif

#include "sensor_manager.h"
#include "madgwick.h"
#include "fusion/Fusion.h"
#include "kv_store.h"
#include "app_diag.h"
#include "devshot.h"

/* set while the PPP link carries binary frames */
extern volatile bool g_net_silent;

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <math.h>

#define LSM6DSL_DEV   "/dev/lsm6dsl0"
#define SENSOR_I2C    "/dev/i2c1"

#define G_MS2  9.80665f

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static inline float wrap_deg(float deg)
{
    while (deg >= 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

static int g_imu_fd = -1;

/* xioTechnologies/Fusion AHRS: industry-standard 9-axis quaternion
 * attitude/heading filter with built-in acceleration and magnetic
 * rejection (handles shaking and interference without custom gating). */
static FusionAhrs s_ahrs;
static bool s_ahrs_init = false;

int sensor_init(sensor_manager_t *mgr)
{
    memset(mgr, 0, sizeof(*mgr));

    mgr->imu.present = false;
    mgr->imu.last_raise_ms = 0;

    g_imu_fd = open(LSM6DSL_DEV, O_RDONLY | O_NONBLOCK);
    if (g_imu_fd < 0)
    {
        printf("[Sensor] %s not available (no IMU)\n", LSM6DSL_DEV);
    }
    else
    {
        /* Start continuous conversion. */
        ioctl(g_imu_fd, SNIOC_START, 0);
        mgr->imu.present = true;
        printf("[Sensor] LSM6DS3 IMU ready\n");
    }

    mag_mmc5603_init(&mgr->mag, SENSOR_I2C);

    /* xioTechnologies/Fusion AHRS: 9-axis quaternion heading.  Convention
     * NED with body X = 12 o'clock / Y = 3 o'clock / Z = into screen makes
     * the yaw equal the bearing of 12 o'clock from magnetic north (the
     * same value the flat formula atan2f(-my,mx) gives on a flat board —
     * validated 311 deg true bearing -> 310 deg).  Acceleration rejection
     * discards shaking; magnetic rejection discards field interference;
     * both recover after rejectionTimeout. */
    FusionAhrsInitialise(&s_ahrs);
    FusionAhrsSettings ahrs_settings = fusionAhrsDefaultSettings;
    /* nominal 20 ms tick; the real per-update period is fed to the filter
     * via FusionAhrsSetSamplePeriod below (loop runs ~28 Hz), so this
     * value only scales the rejection/recovery sample counters */
    ahrs_settings.sampleRate = 28.0f;
    ahrs_settings.convention = FusionConventionNed;
    ahrs_settings.gyroscopeRange = 2000.0f;   /* LSM6DS3 full scale */
    /* Rejection re-enabled after the axis-validation round: sensor axes
     * (accel/gyro X,Y inverted vs the watch frame; mag Z offset fixed)
     * are confirmed, so the attitude tracks correctly and the residual
     * rejection only discards genuine shaking / field interference. */
    ahrs_settings.accelerationRejection = 15.0f;
    ahrs_settings.magneticRejection = 15.0f;
    ahrs_settings.rejectionTimeout = 5.0f;
    FusionAhrsSetSettings(&s_ahrs, &ahrs_settings);
    /* DSH patch: let the magnetometer recover much more slowly than the
     * accelerometer (~20 s vs 5 s) so a disturbed field is not re-accepted
     * every few seconds (each re-acceptance can yank the yaw by 100 deg).
     * Threshold is synced right away so the longer recovery applies from
     * the very first rejection event, not after the first re-arm. */
    s_ahrs.magneticRecoveryTimeout =
        (int32_t)(ahrs_settings.sampleRate * 20.0f);
    s_ahrs.magneticRecoveryThreshold = s_ahrs.magneticRecoveryTimeout;
    s_ahrs_init = true;
    als_ltr303_init(&mgr->als, SENSOR_I2C);

    mgr->backlight_pct = 50;
    mgr->last_fusion_ms = get_time_ms();
    /* first stillness re-calibration ~8 s after boot (so a missed boot
     * calibration is fixed quickly), then every ~45 s */
    mgr->bias_recal_ms = get_time_ms() - 37000;

    return 0;
}

/* gyro zero-bias calibration: average the raw gyro right after boot
 * (removes the largest yaw-drift source).  Only violent motion is
 * excluded (|gyro| < 15 deg/s, |a| 0.8..1.2 g); normal handling averages
 * out and still lands near the true bias.  Skipping the calibration when
 * the user is moving leaves a multi-deg/s residual that drags the yaw off
 * the magnetometer (measured: 1.3/3.3 deg/s residuals -> heading off by
 * 30 deg). */
static void gyro_calibrate_bias(sensor_manager_t *mgr)
{
    struct lsm6dsl_sensor_data_s sdata;
    double sum_x = 0, sum_y = 0, sum_z = 0;
    int n = 0, tried = 0;
    uint32_t t0 = get_time_ms();

    while (get_time_ms() - t0 < 2500 && tried < 100)
    {
        if (ioctl(g_imu_fd, SNIOC_LSM6DSLSENSORREAD,
                  (unsigned long)&sdata) == 0)
        {
            tried++;
            float gx = sdata.g_x_data / 1000.0f;
            float gy = sdata.g_y_data / 1000.0f;
            float gz = sdata.g_z_data / 1000.0f;
            float amag = sqrtf((float)sdata.x_data * sdata.x_data +
                               (float)sdata.y_data * sdata.y_data +
                               (float)sdata.z_data * sdata.z_data);
            if (fabsf(gx) < 15.0f && fabsf(gy) < 15.0f &&
                fabsf(gz) < 15.0f && amag > 800.0f && amag < 1200.0f)
            {
                sum_x += sdata.g_x_data;
                sum_y += sdata.g_y_data;
                sum_z += sdata.g_z_data;
                n++;
            }
        }
        usleep(20000);
    }
    if (n > 10)
    {
        mgr->gyro_bias_z = (float)(sum_z / n) / 1000.0f;
        mgr->gyro_bias_x = (float)(sum_x / n) / 1000.0f;
        mgr->gyro_bias_y = (float)(sum_y / n) / 1000.0f;
    }
        /* bias stored in the raw chip frame; the sample path negates X/Y,
         * so print the equivalent body-frame bias */
        printf("[IMU] gyro bias=(%.2f,%.2f,%.2f) deg/s (n=%d)\n",
           -mgr->gyro_bias_x, -mgr->gyro_bias_y, mgr->gyro_bias_z, n);
}

static void sensor_update_imu(sensor_manager_t *mgr)
{
    struct lsm6dsl_sensor_data_s sdata;
    imu_t *imu = &mgr->imu;
    float accel_mag, accel_diff, tilt;
    uint32_t now = get_time_ms();

    if (!imu->present || g_imu_fd < 0)
        return;

    /* one-time zero-bias calibration at the first sample */
    if (!mgr->gyro_bias_calibrated)
    {
        mgr->gyro_bias_calibrated = true;
        gyro_calibrate_bias(mgr);
    }

    if (ioctl(g_imu_fd, SNIOC_LSM6DSLSENSORREAD, (unsigned long)&sdata) == 0)
    {
        /* The LSM6DS3 is mounted rotated 180 degrees in-plane: the chip's
         * X/Y axes point toward 6/9 o'clock while the watch frame uses
         * X = 12 o'clock, Y = 3 o'clock, Z = into the screen.  Negate the
         * chip X/Y so all sensor data (and the Fusion feed) lives in the
         * watch frame; Z already matches.  (Verified empirically: raising
         * 12 o'clock must give +ax, holding 12 o'clock up must give +ax
         * ~= +1 g; both read inverted before this remap.) */
        imu->ax = -sdata.x_data * G_MS2 / 1000.0f;
        imu->ay = -sdata.y_data * G_MS2 / 1000.0f;
        imu->az =  sdata.z_data * G_MS2 / 1000.0f;
        imu->gx = -(sdata.g_x_data / 1000.0f - mgr->gyro_bias_x);
        imu->gy = -(sdata.g_y_data / 1000.0f - mgr->gyro_bias_y);
        imu->gz =  (sdata.g_z_data / 1000.0f - mgr->gyro_bias_z);
        imu->temperature_c = (float)sdata.temperature;
        imu->raw_ax = sdata.x_data;
        imu->raw_ay = sdata.y_data;
        imu->raw_az = sdata.z_data;
        imu->raw_gx = sdata.g_x_data;
        imu->raw_gy = sdata.g_y_data;
        imu->raw_gz = sdata.g_z_data;

        /* integrate the gyro around the GRAVITY axis (the horizontal
         * yaw of the whole body), not the raw Z axis: on the wrist the
         * raw Z mixes arm swings with body turns.  Project the gyro
         * vector onto the accelerometer-derived gravity direction, so a
         * body turn changes the bearing but swinging the arm does not.
         *   - deadband: ignore < 15 deg/s (stillness / slow sway)
         *   - clamp per-sample delta to +-2 deg
         *   - low-pass the displayed value */
        if (imu->last_sample_ms != 0)
        {
            float dt = (float)(now - imu->last_sample_ms) / 1000.0f;
            if (dt > 0.0f && dt < 0.5f)
            {
                float amag = sqrtf(imu->ax * imu->ax +
                                   imu->ay * imu->ay +
                                   imu->az * imu->az);
                float w_yaw = 0.0f;

                if (amag > 0.5f)
                {
                    /* Accel stability gate: the accelerometer reads pure
                     * gravity only while |a| is close to 1 g.  Under
                     * shaking/swinging, linear acceleration corrupts the
                     * gravity direction, which both the gyro yaw
                     * projection AND the mag tilt compensation depend on.
                     * While unstable we ZERO the yaw rate instead of
                     * projecting the gyro onto a stale/wrong axis; a
                     * controlled flat turn keeps |a| ~= 1 g and keeps the
                     * rate honest.  The fusion layer re-anchors the
                     * heading to the magnetometer on return to stability. */
                    bool accel_stable = fabsf(amag - 9.81f) < 2.5f;
                    if (accel_stable)
                    {
                        mgr->gravity_x = imu->ax / amag;
                        mgr->gravity_y = imu->ay / amag;
                        mgr->gravity_z = imu->az / amag;
                        mgr->gravity_valid = true;
                    }
                    if (mgr->gravity_valid && accel_stable)
                    {
                        /* gyro (deg/s) projected on the stable gravity
                         * axis = horizontal yaw rate */
                        w_yaw = imu->gx * mgr->gravity_x +
                                imu->gy * mgr->gravity_y +
                                imu->gz * mgr->gravity_z;
                    }
                }
                /* deadband just above the post-bias noise floor (~1 deg/s)
                 * so slow controlled turns still register in the yaw rate */
                if (w_yaw > -3.0f && w_yaw < 3.0f)
                    w_yaw = 0.0f;
                mgr->gyro_yaw_rate = w_yaw;
            }
            mgr->gyro_yaw_valid = true;
        }
        imu->last_sample_ms = now;

        accel_mag = sqrtf(imu->ax * imu->ax + imu->ay * imu->ay +
                          imu->az * imu->az);
        accel_diff = fabsf(accel_mag - G_MS2);

        /* pitch from accelerometer (deg) */
        tilt = atan2f(imu->ax, sqrtf(imu->ay * imu->ay + imu->az * imu->az)) *
               57.29578f;
        imu->pitch = tilt;
        imu->roll  = atan2f(imu->ay,
                            sqrtf(imu->ax * imu->ax + imu->az * imu->az)) *
                     57.29578f;

        /* running / moving detection */
        if (accel_diff < 0.15f * G_MS2)
        {
            if (imu->moving)
            {
                imu->stationary_start_ms = now;
                imu->moving = false;
            }
        }
        else
        {
            imu->moving = true;
            imu->stationary_start_ms = 0;
        }

        /* stillness-based gyro bias re-calibration: while the watch truly
         * rests, the remapped gyro readings are pure bias error.
         * Re-averaging them every ~45 s keeps the gyro-driven yaw from
         * slowly drifting during the (frequent) periods when the
         * magnetometer is rejected by interference.  Non-blocking.
         * IMPORTANT: the stillness test requires BOTH |a| ~= 1 g AND
         * small gyro magnitudes — a pure rotation has |a| ~= 1 g and would
         * otherwise be mistaken for stillness, baking real rotation rates
         * into the bias (seen: a shake corrupted the bias to
         * (4.45,-10.87,2.93) deg/s and spun the attitude). */
        if (!imu->moving && now - mgr->bias_recal_ms >
                (mgr->mag_dropped ? 20000u : 45000u))
        {
            /* gate at 4 deg/s on X/Y: excludes real motion but still
             * allows a residual bias of up to ~4 deg/s (e.g. a missed boot
             * calibration) to be corrected.  Z is stricter (1 deg/s): gyro
             * Z is the yaw-drift contributor, and a slow real rotation
             * (2-3 deg/s sitting turn, |a| ~= 1 g) would otherwise be
             * mistaken for stillness and baked into gyro_bias_z during the
             * magnetometer-masked periods. */
            if (fabsf(imu->gx) < 4.0f && fabsf(imu->gy) < 4.0f &&
                fabsf(imu->gz) < 1.0f)
            {
                if (mgr->bias_sum_n == 0)
                    mgr->bias_accum_start = now;
                mgr->bias_sum_x += imu->gx;
                mgr->bias_sum_y += imu->gy;
                mgr->bias_sum_z += imu->gz;
                mgr->bias_sum_n++;
                if (mgr->bias_sum_n >= 40 &&
                    now - mgr->bias_accum_start > 1200)
                {
                    float bx = mgr->bias_sum_x / (float)mgr->bias_sum_n;
                    float by = mgr->bias_sum_y / (float)mgr->bias_sum_n;
                    float bz = mgr->bias_sum_z / (float)mgr->bias_sum_n;
                    /* gx = -(raw/1000 - bias_x): cancel residual bx by
                     * bias_x -= bx;  gz = raw/1000 - bias_z: bias_z += bz */
                    mgr->gyro_bias_x -= bx;
                    mgr->gyro_bias_y -= by;
                    mgr->gyro_bias_z += bz;
                    mgr->bias_sum_n = 0;
                    mgr->bias_recal_ms = now;
                    printf("[IMU] gyro bias re-cal (%.2f,%.2f,%.2f) deg/s\n",
                           -mgr->gyro_bias_x, -mgr->gyro_bias_y,
                           mgr->gyro_bias_z);
                }
            }
            else
            {
                mgr->bias_sum_n = 0;   /* motion burst: restart the window */
            }
        }
        else
        {
            mgr->bias_sum_n = 0;
        }

        /* view lock: freeze map rotation while the runner is moving, so
         * the route preview does not jitter; unlock 3 s after stopping. */
        if (imu->moving)
        {
            imu->view_locked = true;
        }
        else if (imu->stationary_start_ms &&
                 now - imu->stationary_start_ms >
                     IMU_VIEW_LOCK_STATIONARY_S * 1000)
        {
            imu->view_locked = false;
        }

        /* raise-to-wake: a wrist lift shows as a short accel spike with
         * the watch tilted towards a viewing angle */
        imu->raise_detected = false;
        if (accel_diff > IMU_RAISE_ACCEL_MS2 &&
            tilt > -60.0f && tilt < 60.0f &&
            now - imu->last_raise_ms > IMU_RAISE_COOLDOWN_MS)
        {
            imu->raise_detected = true;
            imu->last_raise_ms = now;
        }
    }
}

static void sensor_update_als(sensor_manager_t *mgr)
{
    uint16_t lux;

    if (!mgr->als.present)
        return;

    if (als_ltr303_read(&mgr->als) < 0)
        return;

    lux = mgr->als.filtered_lux;

    /* brightness %: outdoor daylight gets full brightness, night dims */
    if (lux < 10)         mgr->backlight_pct = 50;
    else if (lux < 100)   mgr->backlight_pct = 60;
    else if (lux < 500)   mgr->backlight_pct = 70;
    else if (lux < 2000)  mgr->backlight_pct = 85;
    else if (lux < 10000) mgr->backlight_pct = 95;
    else                  mgr->backlight_pct = 100;
}

/* Tilt-compensated compass heading from accel + mag, both already in the
 * watch frame (X = 12 o'clock, Y = 3 o'clock, Z = into the screen).
 * Returns the bearing of the 12 o'clock direction, degrees CW from
 * magnetic north.  Deterministic replacement for the Madgwick quaternion
 * path (whose NED accel/mag sign conventions never matched this board).
 * The accelerometer measures proper acceleration (reads -g at rest), so
 * the gravity (down) direction is -a. */
static float tilt_compass_heading(sensor_manager_t *mgr, float fallback)
{
    float dx, dy, dz;
    float mx = mgr->mag.x_g, my = mgr->mag.y_g, mz = mgr->mag.z_g;
    float dot, hx, hy, hz, hn, ex, ey, ez, fx, fy, fz, fn, h;

    if (mgr->gravity_valid)
    {
        /* stable gravity (updated only while |a| ~= 1g): down = -a/|a| */
        dx = -mgr->gravity_x;
        dy = -mgr->gravity_y;
        dz = -mgr->gravity_z;
    }
    else
    {
        float dn;
        dx = -mgr->imu.ax; dy = -mgr->imu.ay; dz = -mgr->imu.az;
        dn = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dn < 1.0f)
            return fallback;
        dx /= dn; dy /= dn; dz /= dn;         /* down unit vector */
    }

    /* field projected onto the horizontal plane */
    dot = mx * dx + my * dy + mz * dz;
    hx = mx - dot * dx;
    hy = my - dot * dy;
    hz = mz - dot * dz;
    hn = sqrtf(hx * hx + hy * hy + hz * hz);
    if (hn < 0.5f)
        return fallback;                       /* field ~vertical */
    hx /= hn; hy /= hn; hz /= hn;              /* north, horizontal */

    /* east = down x north (right-handed watch frame) */
    ex = dy * hz - dz * hy;
    ey = dz * hx - dx * hz;
    ez = dx * hy - dy * hx;

    /* 12 o'clock (+X) projected onto the horizontal plane */
    fx = 1.0f - dx * dx;
    fy = -dx * dy;
    fz = -dx * dz;
    fn = sqrtf(fx * fx + fy * fy + fz * fz);
    if (fn < 0.1f)
        return fallback;                       /* 12 o'clock vertical */
    fx /= fn; fy /= fn; fz /= fn;

    h = atan2f(fx * ex + fy * ey + fz * ez,
               fx * hx + fy * hy + fz * hz) * 57.29578f;
    if (h < 0.0f)
        h += 360.0f;
    return h;
}

/* ------------------------------------------------------------------ *
 * background auto-calibration (program-internal)
 * Learns the hard-iron offset (and, when a full 3D sweep is seen, the
 * per-axis soft-iron scale) WITHOUT any user gesture: raw samples are
 * collected whenever the magnetometer produces a valid reading, and a
 * least-squares sphere fit (centre = offset) is run periodically on the
 * recent ring.  Results are gated on coverage/diversity so a one-pose
 * desk sample can never trash the calibration, blended gently into the
 * live offset, and stored to KV for the current boot.
 * ------------------------------------------------------------------ */

#define AC_RING        256          /* recent raw samples (chip frame) */
#define AC_MIN_SPAN    2500.0f      /* min counts swept on each axis */
#define AC_FULL_SPAN   12000.0f     /* span needed to trust the scale  */
#define AC_RESET_NEW   160          /* new samples before a refit */

static int32_t s_ac[AC_RING][3];
static int32_t s_ac_c[AC_RING][3];    /* contiguous copy for the fit */
static uint16_t s_ac_write;
static uint16_t s_ac_count;
static uint32_t s_ac_last_ms;
static uint32_t s_ac_last_fit_ms;
static uint32_t s_ac_last_apply_ms;
static uint32_t s_ac_last_attempt_log_ms;
static int s_ac_new_since_fit;

/* solve for the sphere centre: minimise |m - o|^2 = R^2 over all samples
 * via the linear system 2*m^T o + c = |m|^2  (Gauss-Jordan, 4x4). */
static bool ac_solve_offset(int32_t (*r)[3], int n,
                            float *ox, float *oy, float *oz,
                            float *resid)
{
    double A[4][5] = { {0} };
    int i, k;

    for (i = 0; i < n; i++)
    {
        double x = r[i][0], y = r[i][1], z = r[i][2];
        double u[4] = { 2.0 * x, 2.0 * y, 2.0 * z, 1.0 };
        double mm = x * x + y * y + z * z;
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++)
                A[a][b] += u[a] * u[b];
        for (int a = 0; a < 4; a++)
            A[a][4] += u[a] * mm;
    }

    for (k = 0; k < 4; k++)
    {
        int piv = k;
        double mx = fabs(A[k][k]);
        for (i = k + 1; i < 4; i++)
            if (fabs(A[i][k]) > mx) { mx = fabs(A[i][k]); piv = i; }
        if (mx < 1e-12)
            return false;
        if (piv != k)
            for (int c = 0; c < 5; c++)
            {
                double t = A[k][c]; A[k][c] = A[piv][c]; A[piv][c] = t;
            }
        for (i = 0; i < 4; i++)
        {
            if (i == k) continue;
            double f = A[i][k] / A[k][k];
            for (int c = k; c < 5; c++)
                A[i][c] -= f * A[k][c];
        }
    }
    {
        double o[4];
        double acc = 0.0;
        for (i = 0; i < 4; i++)
            o[i] = A[i][4] / A[i][i];
        for (i = 0; i < n; i++)
        {
            double x = r[i][0], y = r[i][1], z = r[i][2];
            double pred = 2.0 * (x * o[0] + y * o[1] + z * o[2]) + o[3];
            double mm = x * x + y * y + z * z;
            double e = (pred - mm) / (mm > 1e-9 ? mm : 1e-9);
            acc += e * e;
        }
        *resid = (float)sqrt(acc / n);
        *ox = (float)o[0]; *oy = (float)o[1]; *oz = (float)o[2];
        return true;
    }
}

static void ac_store_kv(mag_mmc5603_t *mag)
{
    kv_set_int("mag.ox", (int32_t)(mag->offset_x * 1000.0f));
    kv_set_int("mag.oy", (int32_t)(mag->offset_y * 1000.0f));
    kv_set_int("mag.oz", (int32_t)(mag->offset_z * 1000.0f));
    kv_set_int("mag.sx", (int32_t)(mag->scale_x * 1000.0f));
    kv_set_int("mag.sy", (int32_t)(mag->scale_y * 1000.0f));
    kv_set_int("mag.sz", (int32_t)(mag->scale_z * 1000.0f));
}

/* called once per valid mag sample */
static void mag_auto_cal_update(sensor_manager_t *mgr, uint32_t now)
{
    mag_mmc5603_t *mag = &mgr->mag;
    int n, i;
    int32_t mn[3], mx[3];
    float span[3], avg;
    float ocx, ocy, ocz, resid;
    float md;

    (void)mgr;

    /* push this valid sample into the ring (chip-frame raw counts) */
    if (now - s_ac_last_ms < 40)
        return;                      /* max ~25 Hz collection */
    s_ac_last_ms = now;

    /* never learn from a disturbed field (dropped by the sanity gate) */
    {
        float fn = sqrtf(mag->x_g * mag->x_g + mag->y_g * mag->y_g +
                         mag->z_g * mag->z_g);
        if (fn < 15.0f || fn > 120.0f)
            return;
    }

    s_ac[s_ac_write][0] = mag->x_raw;
    s_ac[s_ac_write][1] = mag->y_raw;
    s_ac[s_ac_write][2] = mag->z_raw;
    s_ac_write = (uint16_t)((s_ac_write + 1) % AC_RING);
    if (s_ac_count < AC_RING)
        s_ac_count++;
    s_ac_new_since_fit++;

    /* refit every ~6 s once enough fresh samples arrived */
    if (s_ac_new_since_fit < AC_RESET_NEW)
        return;
    if (s_ac_last_fit_ms != 0 && (uint32_t)(now - s_ac_last_fit_ms) < 6000)
        return;
    s_ac_last_fit_ms = now;

    /* do not refit while actually moving: arm swings / walking would
     * contaminate the fit with transient fields and motion noise */
    if (mgr->imu.moving || fabsf(mgr->gyro_yaw_rate) > 18.0f)
        return;

    n = (int)s_ac_count;
    for (int a = 0; a < 3; a++)
    {
        mn[a] = 0x7FFFFFFF;
        mx[a] = -0x7FFFFFFF;
    }
    {
        /* linearise the ring (oldest -> newest) */
        int first = ((int)s_ac_write - n + AC_RING) % AC_RING;
        for (i = 0; i < n; i++)
        {
            int j = (first + i) % AC_RING;
            for (int a = 0; a < 3; a++)
                s_ac_c[i][a] = s_ac[j][a];
        }
        for (i = 0; i < n; i++)
        {
            for (int a = 0; a < 3; a++)
            {
                int32_t v = s_ac_c[i][a];
                if (v < mn[a]) mn[a] = v;
                if (v > mx[a]) mx[a] = v;
            }
        }
    }

    span[0] = (float)(mx[0] - mn[0]);
    span[1] = (float)(mx[1] - mn[1]);
    span[2] = (float)(mx[2] - mn[2]);

    /* coverage gate: need a real 3D sweep; flat-only samples never
     * update the offset (Z unobservable -> garbage) */
    if (span[0] < AC_MIN_SPAN || span[1] < AC_MIN_SPAN ||
        span[2] < AC_MIN_SPAN)
    {
        if (now - s_ac_last_attempt_log_ms > 30000)
        {
            s_ac_last_attempt_log_ms = now;
            printf("[AC] waiting 3D motion span=(%.0f,%.0f,%.0f) n=%d\n",
                   span[0], span[1], span[2], n);
        }
        return;
    }

    if (!ac_solve_offset(s_ac_c, n, &ocx, &ocy, &ocz, &resid))
        return;

    /* diversity: mean of unit vectors (about the fitted centre) should
     * be near zero for a well-distributed sweep */
    {
        double sx = 0, sy = 0, sz = 0;
        for (i = 0; i < n; i++)
        {
            double dx = s_ac_c[i][0] - ocx, dy = s_ac_c[i][1] - ocy,
                   dz = s_ac_c[i][2] - ocz;
            double d = sqrt(dx * dx + dy * dy + dz * dz);
            if (d > 1e-6)
            {
                sx += dx / d; sy += dy / d; sz += dz / d;
            }
        }
        sx /= n; sy /= n; sz /= n;
        md = (float)sqrt(sx * sx + sy * sy + sz * sz);
    }

    if (md >= 0.40f ||
        fabsf(ocx - mag->offset_x) > 20000.0f ||
        fabsf(ocy - mag->offset_y) > 20000.0f ||
        fabsf(ocz - mag->offset_z) > 20000.0f)
    {
        if (now - s_ac_last_attempt_log_ms > 30000)
        {
            s_ac_last_attempt_log_ms = now;
            printf("[AC] reject md=%.2f o=(%.0f,%.0f,%.0f) res=%.3f\n",
                   md, ocx, ocy, ocz, resid);
        }
        return;
    }

    /* only apply when the fit moved us a meaningful amount, and never
     * more often than once per 45 s - repeated tiny fits on a noisy
     * table would continuously tug the heading */
    {
        float dx = ocx - mag->offset_x;
        float dy = ocy - mag->offset_y;
        float dz = ocz - mag->offset_z;

        if (fabsf(dx) < 600.0f && fabsf(dy) < 600.0f &&
            fabsf(dz) < 600.0f)
            return;
        if (s_ac_last_apply_ms != 0 &&
            (uint32_t)(now - s_ac_last_apply_ms) < 45000)
            return;

        /* blend gently towards the fitted centre */
        mag->offset_x += 0.25f * dx;
        mag->offset_y += 0.25f * dy;
        mag->offset_z += 0.25f * dz;
        s_ac_last_apply_ms = now;

        /* full 3D sweep seen? also refresh the per-axis scale */
        if (span[0] >= AC_FULL_SPAN && span[1] >= AC_FULL_SPAN &&
            span[2] >= AC_FULL_SPAN)
        {
            avg = (span[0] + span[1] + span[2]) / 3.0f;
            mag->scale_x = 0.5f * mag->scale_x + 0.5f * (avg / span[0]);
            mag->scale_y = 0.5f * mag->scale_y + 0.5f * (avg / span[1]);
            mag->scale_z = 0.5f * mag->scale_z + 0.5f * (avg / span[2]);
            printf("[AC] scale=(%.2f,%.2f,%.2f)\n",
                   mag->scale_x, mag->scale_y, mag->scale_z);
        }

        mag->calibrated = true;
        ac_store_kv(mag);
        printf("[AC] fit off=(%.0f,%.0f,%.0f) span=(%.0f,%.0f,%.0f)"
               " md=%.2f res=%.3f APPLIED\n",
               mag->offset_x, mag->offset_y, mag->offset_z,
               span[0], span[1], span[2], md, resid);
    }

    s_ac_new_since_fit = 0;
}

void sensor_update(sensor_manager_t *mgr)
{
    uint32_t now = get_time_ms();

#ifdef CONFIG_ARCH_SIM
    /* Host simulator: no sensor devices, feed simulated 9-axis data so
     * the whole render/interact pipeline can be exercised AND the
     * Madgwick compass algorithm validated headless: the watch rotates
     * about the world vertical at a known rate, the magnetic field is
     * rotated into the watch frame, and the fused heading is compared
     * against the ground-truth yaw. */
    if (!mgr->mag.present && !mgr->imu.present)
    {
        static uint32_t t0;
        static float sim_yaw = 0.0f;
        float yaw_rate = 12.0f;   /* deg/s */
        float dt, rad;
        const float Bx = 30.0f;

        if (t0 == 0)
            t0 = now;
        dt = (float)(now - t0) / 1000.0f;
        sim_yaw = fmodf(dt * yaw_rate, 360.0f);

        rad = sim_yaw * 0.0174533f;
        /* earth field in the watch frame at bearing sim_yaw (the mag
         * driver remaps the chip into this frame on hardware):
         * X = 12 o'clock, Y = right, Z = into screen; ~55 deg dip */
        mgr->mag.x_g = Bx * cosf(rad);
        mgr->mag.y_g = -Bx * sinf(rad);
        mgr->mag.z_g = 45.0f;
        mgr->mag.present = true;
        mgr->mag.healthy = true;

        /* accelerometer: flat on the table, reads -g (proper accel up) */
        mgr->imu.ax = 0.0f;
        mgr->imu.ay = 0.0f;
        mgr->imu.az = -9.81f;
        mgr->imu.gx = 0.0f;
        mgr->imu.gy = 0.0f;
        mgr->imu.gz = yaw_rate;

        /* same tilt-compensated compass as the hardware path */
        mgr->heading_deg = tilt_compass_heading(mgr, 0.0f);
        mgr->mag_healthy = true;
        mgr->heading_valid = true;

        /* validation printout: sim yaw vs fused yaw, error */
        {
            static int dbg = 0;
            if (dbg < 2000)
            {
                float err = mgr->heading_deg - sim_yaw;
                while (err > 180.0f) err -= 360.0f;
                while (err < -180.0f) err += 360.0f;
                if (dbg % 10 == 0)
                    printf("[CompassSim] sim=%.1f fused=%.1f err=%.2f\n",
                           sim_yaw, mgr->heading_deg, err);
                dbg++;
            }
        }

        mgr->als.present = true;
        mgr->als.filtered_lux = 300;
        mgr->backlight_pct = 45;
        return;
    }
#endif

    sensor_update_imu(mgr);
    if (mgr->mag.calib_running)
        mag_mmc5603_calib_step(&mgr->mag);
    else
    {
        int mrc = mag_mmc5603_read(&mgr->mag);
        if (mrc == 0)
        {
            /* background auto-calibration samples this valid reading */
            mag_auto_cal_update(mgr, now);
        }
    }

    {
        static int dbg = 0;
        if (dbg < 40)
        {
#if APP_DIAG_VERBOSE
#if HUANGSHAN_DEV_SHOT
            if (!g_print_silent)
#endif
            printf("[Fuse] yaw=%.0f mx=%.0f my=%.0f mz=%.0f ok=%d cal=%d off=(%.0f,%.0f,%.0f) gz=%.1f\n",
                   mgr->heading_deg,
                   mgr->mag.x_g, mgr->mag.y_g, mgr->mag.z_g,
                   (int)mgr->mag.healthy, (int)mgr->mag.calibrated,
                   mgr->mag.offset_x, mgr->mag.offset_y, mgr->mag.offset_z,
                   mgr->imu.gz);
            dbg++;
#endif
        }
    }

    /* Compass heading: xioTechnologies/Fusion AHRS (industry-standard).
     * Fuses gyro + accel + magnetometer into a quaternion; the built-in
     * acceleration rejection discards linear-acceleration-corrupted
     * samples (shaking/swinging) and the magnetic rejection discards
     * field interference — the two failure modes that plagued the
     * hand-rolled fusion.  Units: gyro deg/s, accel g, mag any calibrated
     * unit (we feed the driver-calibrated uT). */
    if (s_ahrs_init)
    {
        /* The sensor task runs slower than the nominal 20 ms (mag polling
         * + IPC + scheduling take ~15 ms more), so a fixed sample period
         * under-integrates the gyro by ~40 % (measured: a 53 deg lift
         * integrated only 24 deg).  Feed the real elapsed time each
         * update so gyro integration and the startup ramp are exact. */
        uint32_t now_ms = get_time_ms();
        float dt = (float)(now_ms - mgr->last_fusion_ms) / 1000.0f;
        mgr->last_fusion_ms = now_ms;
        if (dt > 0.001f && dt < 0.5f)
            FusionAhrsSetSamplePeriod(&s_ahrs, dt);

        const FusionVector gyroscope = {
            .axis = { mgr->imu.gx, mgr->imu.gy, mgr->imu.gz },
        };
        const FusionVector accelerometer = {
            .axis = { mgr->imu.ax / G_MS2, mgr->imu.ay / G_MS2,
                      mgr->imu.az / G_MS2 },
        };
        const FusionVector magnetometer = {
            .axis = { mgr->mag.x_g, mgr->mag.y_g, mgr->mag.z_g },
        };

        mgr->mag_healthy = (mgr->mag.present && mgr->mag.healthy);

        /* Magnetic-field sanity gate: when the ambient field is heavily
         * disturbed (hand/desk/phone steel absorbs or redirects it), the
         * magnetometer reports a wrong DIRECTION with an abnormally weak
         * magnitude.  Feeding it makes the fused yaw snap toward the wrong
         * heading whenever the rejection/recovery toggles (measured: -100
         * deg jumps while rotating hand-held, and a wrong "initial
         * position" at rest).  If the total field magnitude is far from
         * the local ~50 uT, drop the mag and run gyro+accel only (the
         * stillness bias re-calibration keeps the yaw from drifting much);
         * re-enable with hysteresis once the field looks sane again.  The
         * first ~2.5 s after boot always use the mag so the heading
         * initialises even at a weak spot. */
        {
            float mnorm = sqrtf(mgr->mag.x_g * mgr->mag.x_g +
                                mgr->mag.y_g * mgr->mag.y_g +
                                mgr->mag.z_g * mgr->mag.z_g);
            static bool mag_suspect;
            static uint32_t boot_ms;
            static bool boot_init;
            bool in_boot;

            if (!boot_init)
            {
                boot_ms = get_time_ms();
                boot_init = true;
            }
            in_boot = (get_time_ms() - boot_ms) < 2500;

            if (in_boot)
                mag_suspect = false;
            else if (mnorm < 12.0f || mnorm > 130.0f)
                mag_suspect = true;
            else if (mnorm > 18.0f)
                mag_suspect = false;

            /* Feed decision.  In the boot window we anchor the heading on
             * the magnetometer even when the field is weak (as long as it
             * is not extreme) - this makes the "gyro only" fallback start
             * from a REAL magnetic heading instead of an arbitrary 0 deg.
             * Afterwards the mag must be both sane and not suspect. */
            bool feed_mag = in_boot
                                ? (mgr->mag.present && mnorm > 4.0f &&
                                   mnorm < 200.0f)
                                : (mgr->mag_healthy && !mag_suspect);

            if (feed_mag)
            {
                FusionAhrsUpdate(&s_ahrs, gyroscope, accelerometer,
                                 magnetometer);
            }
            else
            {
                /* no/invalid or suspicious magnetometer: gyro+accel only
                 * (drifts slowly, but keeps the anchored heading) */
                FusionAhrsUpdateNoMagnetometer(&s_ahrs, gyroscope,
                                               accelerometer);
            }
            /* expose the decision for the UI (MAG vs GYRO source label) */
            mgr->mag_dropped = !feed_mag;
        }

        FusionEuler euler = FusionQuaternionToEuler(
            FusionAhrsGetQuaternion(&s_ahrs));
        float yaw = euler.angle.yaw;              /* degrees, NED */
        while (yaw >= 360.0f) yaw -= 360.0f;
        while (yaw < 0.0f) yaw += 360.0f;
        mgr->heading_deg = yaw;
        mgr->heading_valid = true;

        /* 2 Hz diagnostic: Fusion yaw/roll/pitch vs the flat reference
         * formula; raw = pre-remap chip values; ea/em = residual errors */
#if APP_DIAG_VERBOSE
        {
            static uint32_t dbg_ms;
            if (now - dbg_ms >= 500 && !g_net_silent
#if HUANGSHAN_DEV_SHOT
                && !g_print_silent
#endif
               )
            {
                dbg_ms = now;
                float flat = atan2f(-mgr->mag.y_g, mgr->mag.x_g) *
                             57.29578f;
                if (flat < 0.0f) flat += 360.0f;
                FusionAhrsFlags flags = FusionAhrsGetFlags(&s_ahrs);
                FusionAhrsInternalStates st =
                    FusionAhrsGetInternalStates(&s_ahrs);
                printf("[Fuse] yaw=%.0f roll=%.0f pit=%.0f flat=%.0f "
                       "ax=%.2f ay=%.2f az=%.2f gx=%.1f gy=%.1f gz=%.1f "
                       "raw=(%d,%d,%d,%d,%d,%d) ea=%.0f em=%.0f "
                       "ar=%d mr=%d drp=%d\n",
                       mgr->heading_deg, euler.angle.roll, euler.angle.pitch,
                       flat,
                       mgr->imu.ax, mgr->imu.ay, mgr->imu.az,
                       mgr->imu.gx, mgr->imu.gy, mgr->imu.gz,
                       mgr->imu.raw_ax, mgr->imu.raw_ay, mgr->imu.raw_az,
                       mgr->imu.raw_gx, mgr->imu.raw_gy, mgr->imu.raw_gz,
                       st.accelerationError, st.magneticError,
                       (int)flags.accelerationRecovery,
                       (int)flags.magneticRecovery,
                       (int)mgr->mag_dropped);
            }
        }
#endif
    }

    if (now - mgr->last_als_ms >= ALS_SAMPLE_INTERVAL_MS)
    {
        sensor_update_als(mgr);
        mgr->last_als_ms = now;
    }
}

float sensor_get_heading(sensor_manager_t *mgr)
{
    return mgr->heading_deg;
}

bool sensor_detect_raise(sensor_manager_t *mgr)
{
    return mgr->imu.raise_detected;
}

bool sensor_view_locked(sensor_manager_t *mgr)
{
    return mgr->imu.view_locked;
}

uint8_t sensor_get_backlight(sensor_manager_t *mgr)
{
    return mgr->backlight_pct;
}

void sensor_calibrate_magnetometer(sensor_manager_t *mgr,
                                   uint32_t duration_ms)
{
    mag_mmc5603_start_calib(&mgr->mag, duration_ms);
}

bool sensor_mag_calibrating(sensor_manager_t *mgr)
{
    return mgr->mag.calib_running;
}

void sensor_deinit(sensor_manager_t *mgr)
{
    if (g_imu_fd >= 0)
    {
        ioctl(g_imu_fd, SNIOC_STOP, 0);
        close(g_imu_fd);
        g_imu_fd = -1;
    }
    mag_mmc5603_deinit(&mgr->mag);
    als_ltr303_deinit(&mgr->als);
}
