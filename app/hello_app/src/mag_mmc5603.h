/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mag_mmc5603.h - MMC5603NJ 3-axis magnetometer over the NuttX I2C char
 * driver (/dev/i2c1, the sensor bus of the Huangshan Pi).
 */

#ifndef __MAG_MMC5603_H
#define __MAG_MMC5603_H

#include <stdint.h>
#include <stdbool.h>

#define MMC5603_I2C_ADDR      0x30
#define MMC5603_PRODUCT_ID    0x10

/* Compile-time calibration defaults.  SINGLE SOURCE OF TRUTH is
 * mag_mmc5603.c (the values there are applied to the chip data; the old
 * copies of these macros were removed from this header to avoid a hidden
 * macro redefinition shadowing).  Do not re-add them here. */

typedef struct {
    int      fd;              /* /dev/i2c1 */
    uint8_t  i2c_addr;        /* 0x30 or 0x31 */
    bool     present;
    int32_t  x_raw, y_raw, z_raw; /* full 20-bit signed counts (no int16 truncation: calibration min/max uses these) */
    float    x_g, y_g, z_g;   /* Gauss (approx) */
    bool     healthy;         /* field magnitude sane (non-zero) */
    float    otp_comp[3];      /* OTP sensitivity compensation */
    /* async calibration state */
    bool     calib_running;
    uint32_t calib_end_ms;
    int32_t  calib_min_x, calib_max_x;
    int32_t  calib_min_y, calib_max_y;
    int32_t  calib_min_z, calib_max_z;
    /* soft-iron calibration (sphere/ellipsoid fit) */
    float    offset_x, offset_y, offset_z;
    float    scale_x, scale_y, scale_z;
    bool     calibrated;

    /* least-squares sphere fit for the 3D tumble calibration: the normal
     * equations are accumulated per sample (A^T A, A^T b with the row
     * [2x 2y 2z 1] and target x^2+y^2+z^2) and solved once at the end.
     * double is required: the sums reach ~1e12 counts^2. */
    double   fit_ata[4][4];
    double   fit_atb[4];
    uint32_t fit_n;
} mag_mmc5603_t;

/* init: open the I2C bus, probe the chip, issue SET for a clean start.
 * Returns 0 on success (present==true), negative errno otherwise. */
int mag_mmc5603_init(mag_mmc5603_t *mag, const char *i2c_dev);

/* Take one measurement (single-shot TM) and update x/y/z + heading. */
int mag_mmc5603_read(mag_mmc5603_t *mag);

/* Collect min/max over duration_ms and compute offsets+scales. */
void mag_mmc5603_start_calib(mag_mmc5603_t *mag, uint32_t duration_ms);
void mag_mmc5603_calib_step(mag_mmc5603_t *mag);
void mag_mmc5603_calibrate(mag_mmc5603_t *mag, uint32_t duration_ms);

void mag_mmc5603_deinit(mag_mmc5603_t *mag);

#endif /* __MAG_MMC5603_H */
