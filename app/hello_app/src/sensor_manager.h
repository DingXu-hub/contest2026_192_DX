/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * sensor_manager.h - sensor hub on the big core (and IPC-ready for the
 * small core): LSM6DS3 IMU (accel+gyro), MMC5603NJ magnetometer,
 * LTR303 ambient light sensor.
 *
 * Features:
 *   - compass heading (flat magnetometer atan2 + wrap-aware EMA; gyro
 *     only as the no-magnetometer fallback)
 *   - IMU raise-to-wake and view-lock for running
 *   - ALS-driven backlight target
 */

#ifndef __SENSOR_MANAGER_H
#define __SENSOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#include "mag_mmc5603.h"
#include "als_ltr303.h"

#define SENSOR_SAMPLE_INTERVAL_MS   20    /* 50 Hz IMU */
#define ALS_SAMPLE_INTERVAL_MS      500   /* 2 Hz ALS */
#define IMU_RAISE_ACCEL_MS2        4.0f /* accel deviation (m/s^2) that marks a wrist lift */
#define IMU_RAISE_COOLDOWN_MS       2000
#define IMU_VIEW_LOCK_STATIONARY_S  3     /* stable for 3s -> view lock */

#define COMPLEMENTARY_FILTER_ALPHA  0.90f

typedef struct {
    float ax, ay, az;      /* m/s^2 */
    float gx, gy, gz;      /* deg/s */
    float pitch, roll;     /* deg */
    float temperature_c;   /* LSM6DS3 die temperature */
    bool  present;
    bool  moving;
    bool  raise_detected;
    uint32_t last_raise_ms;
    uint32_t stationary_start_ms;
    bool  view_locked;     /* map rotation frozen while running */
    uint32_t last_sample_ms;
    /* last raw chip sample (diagnostics; mg / mdps) */
    int16_t raw_ax, raw_ay, raw_az;
    int16_t raw_gx, raw_gy, raw_gz;
} imu_t;

typedef struct {
    mag_mmc5603_t mag;
    als_ltr303_t  als;
    imu_t         imu;

    float heading_deg;         /* fused heading 0..360 */
    bool  heading_valid;       /* first sample seen */
    bool  mag_healthy;         /* mag produces sane field readings */
    bool  mag_dropped;         /* magnetometer out of the fusion this
                                * update (missing/unhealthy/suspect
                                * field); heading rides on gyro only */
    float gyro_yaw_rate;       /* gravity-projected yaw rate (deg/s) */
    bool  gyro_yaw_valid;
    float gravity_x, gravity_y, gravity_z;  /* stable gravity unit vec */
    bool  gravity_valid;
    float gyro_bias_z;         /* simple gyro bias estimate */
    float gyro_bias_x;
    float gyro_bias_y;
    bool  gyro_bias_calibrated;
    /* stillness-based bias re-calibration state */
    uint32_t bias_recal_ms;    /* last re-calibration / timer base */
    uint32_t bias_accum_start; /* when the accumulation window began */
    float bias_sum_x, bias_sum_y, bias_sum_z;
    uint32_t bias_sum_n;
    uint32_t last_fusion_ms;
    uint32_t last_als_ms;
    uint8_t  backlight_pct;    /* ALS target, 0..100 */
} sensor_manager_t;

int    sensor_init(sensor_manager_t *mgr);
void   sensor_update(sensor_manager_t *mgr);
float  sensor_get_heading(sensor_manager_t *mgr);
bool   sensor_mag_calibrating(sensor_manager_t *mgr);
bool   sensor_detect_raise(sensor_manager_t *mgr);
bool   sensor_view_locked(sensor_manager_t *mgr);
uint8_t sensor_get_backlight(sensor_manager_t *mgr);
void   sensor_calibrate_magnetometer(sensor_manager_t *mgr,
                                     uint32_t duration_ms);
void   sensor_deinit(sensor_manager_t *mgr);

#endif /* __SENSOR_MANAGER_H */
