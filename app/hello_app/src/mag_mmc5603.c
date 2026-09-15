/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * mag_mmc5603.c - MMC5603NJ magnetometer, standard MMC5603 register map
 * (verified against Adafruit_MMC56x3):
 *   CTRL0=0x1B, CTRL1=0x1C, CTRL2=0x1D, STATUS=0x18, ODR=0x1A,
 *   PRODUCT_ID=0x39 (=0x10), 20-bit data at 0x00..0x08.
 * One-shot TM_M reads; status ready bit is STATUS bit6.
 * Calibration runs asynchronously (start_calib + calib_step).
 */

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

#include "mag_mmc5603.h"
#include "kv_store.h"
#include "app_diag.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <math.h>

#define REG_OUT_X_L  0x00
#define REG_STATUS   0x18
#define REG_ODR      0x1A
#define REG_CTRL0    0x1B
#define REG_CTRL1    0x1C
#define REG_CTRL2    0x1D
#define REG_PID      0x39

#define CTRL0_TM_M      0x01
#define CTRL0_TM_T      0x02
#define CTRL0_START_MDT 0x04
#define CTRL0_DO_SET    0x08
#define CTRL0_DO_RESET  0x10
#define CTRL0_AUTO_SR   0x20   /* auto SET/RESET before each measure */
#define CTRL0_CMM_FREQ  0x80

/* STATUS1 (0x18), MMC5603NJ datasheet Rev.B p.8:
 *   bit7 Meas_t_done, bit6 Meas_m_done, bit5 Sat_sensor, bit4 OTP_read_done,
 *   bit3 ST_Fail, bits2..0 factory/internal.
 * NOTE bit5 is NOT a field-saturation flag: the datasheet defines it as the
 * self-test indicator that "keeps low once the device PASS self-test", and
 * real saturation is checked by driving the self-test coil (CTRL1 St_enp/
 * St_enm).  The old code dropped every sample while this bit was high. */
#define STATUS_MAG_READY  0x40   /* bit6: measurement data ready */
#define STATUS_TEMP_READY 0x80   /* bit7 */
#define STATUS_SAT_SENSOR 0x20   /* bit5: self-test signal (low = passed) */
#define STATUS_OTP_DONE   0x10   /* bit4: OTP load done */

/* CTRL1 (0x1C) bits */
#define CTRL1_SW_RST      0x80

/* historical aliases used at init (SET/RESET pulses) */
#define CTRL0_SET    CTRL0_DO_SET
#define CTRL0_RESET  CTRL0_DO_RESET

/* Compile-time (factory) calibration in raw ADC counts.  The board has
 * NO persistent filesystem: /data is tmpfs (lost on reboot) and runtime
 * NOR write/erase hard-faults the SiFli HAL on this XIP build, so the
 * calibration that survives reboots is baked into the binary here.
 *
 * SINGLE SOURCE OF TRUTH: this is the only definition of
 * MAG_CAL_DEFAULT_*.
 *
 * 2026-09-15 re-derivation: the previous values were hand-tuned so that
 * mz ~= 0 while the watch lay flat, i.e. the Z offset was made to absorb
 * the Earth's *vertical* field (the header comment even noted "-44 deg
 * error at 41 deg tilt").  That is physically wrong - a hard-iron offset
 * is a fixed sensor/PCB bias, so subtracting the local vertical field
 * leaves a magnetic vector with no vertical component and therefore a
 * heading error that grows with tilt, and a |B| that changes with
 * orientation (measured 4.5..83 uT with the old values, and Fusion's
 * magnetic error pinned at 90 deg).
 *
 * The values below are the least-squares sphere centre of a 109-sample
 * 3D tumble captured on this unit (raw counts):
 *      centre = (-4169, +1447, -5135)  radius = 7594 counts = 47.5 uT
 * The fitted radius equals the local geomagnetic total field (~47-48 uT
 * at 31 deg N), which is the physical self-check that the fit is right.
 * Per-axis half-spans 6803/6368/7244 give the soft-iron scales. */
#define MAG_CAL_DEFAULT_OX (-4169.0f)
#define MAG_CAL_DEFAULT_OY 1447.0f
#define MAG_CAL_DEFAULT_OZ (-5135.0f)
#define MAG_CAL_DEFAULT_SX 1.000f
#define MAG_CAL_DEFAULT_SY 1.069f
#define MAG_CAL_DEFAULT_SZ 0.939f

static uint32_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
    struct i2c_msg_s msg;
    struct i2c_transfer_s xfer;
    uint8_t buf[2];

    buf[0] = reg;
    buf[1] = val;
    msg.frequency = 100000;
    msg.addr = addr;
    msg.flags = 0;
    msg.buffer = buf;
    msg.length = 2;

    xfer.msgv = &msg;
    xfer.msgc = 1;

    return ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
}

static int i2c_read_regs(int fd, uint8_t addr, uint8_t reg,
                         uint8_t *buf, uint8_t len)
{
    struct i2c_msg_s msgs[2];
    struct i2c_transfer_s xfer;

    msgs[0].frequency = 100000;
    msgs[0].addr = addr;
    msgs[0].flags = 0;
    msgs[0].buffer = &reg;
    msgs[0].length = 1;

    msgs[1].frequency = 100000;
    msgs[1].addr = addr;
    msgs[1].flags = I2C_M_READ;
    msgs[1].buffer = buf;
    msgs[1].length = len;

    xfer.msgv = msgs;
    xfer.msgc = 2;

    return ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
}

int mag_mmc5603_init(mag_mmc5603_t *mag, const char *i2c_dev)
{
    uint8_t pid = 0;
    uint8_t addr;
    static const uint8_t probe_addrs[] = { 0x30, 0x31 };

    memset(mag, 0, sizeof(*mag));
    mag->fd = -1;

    mag->fd = open(i2c_dev, O_RDWR);
    if (mag->fd < 0)
    {
        printf("[Mag] open %s failed\n", i2c_dev);
        return -1;
    }

    mag->present = false;
    for (int i = 0; i < 2; i++)
    {
        addr = probe_addrs[i];
        if (i2c_read_regs(mag->fd, addr, REG_PID, &pid, 1) == 0)
        {
            printf("[Mag] probe 0x%02x: pid=0x%02x (expect 0x10)\n",
                   addr, pid);
            mag->i2c_addr = addr;
            mag->present = true;
            break;
        }
    }
    if (!mag->present)
    {
        printf("[Mag] MMC5603 not found on %s\n", i2c_dev);
        close(mag->fd);
        mag->fd = -1;
        return -1;
    }

    /* Adafruit/MMC5603NJ sequence: SW_RST -> SET/RESET pulse -> one-shot
     * (CTRL0/CTRL2 = 0).  Cmm_freq_en (0x80) is only for continuous mode,
     * so it is not written here; the SET/RESET pulses at reset are what
     * clear the AMR bridge offset before the first on-demand measurement. */
    i2c_write_reg(mag->fd, addr, REG_CTRL1, CTRL1_SW_RST);  /* auto-clears */
    usleep(20000);                     /* datasheet: power-on time 20 ms */
    i2c_write_reg(mag->fd, addr, REG_CTRL0, CTRL0_SET);     /* SET pulse */
    usleep(2000);
    i2c_write_reg(mag->fd, addr, REG_CTRL0, CTRL0_RESET);   /* RESET pulse */
    usleep(2000);
    i2c_write_reg(mag->fd, addr, REG_CTRL0, 0x00);          /* one-shot */
    i2c_write_reg(mag->fd, addr, REG_CTRL2, 0x00);          /* no CMM */

    mag->scale_x = mag->scale_y = mag->scale_z = 1.0f;
    mag->otp_comp[0] = mag->otp_comp[1] = mag->otp_comp[2] = 1.0f;

    /* compile-time default calibration (raw counts) - see the derivation
     * note next to MAG_CAL_DEFAULT_* above.  A session KV copy can still
     * override it until the next reboot. */
    mag->offset_x = MAG_CAL_DEFAULT_OX;
    mag->offset_y = MAG_CAL_DEFAULT_OY;
    mag->offset_z = MAG_CAL_DEFAULT_OZ;
    mag->scale_x  = MAG_CAL_DEFAULT_SX;
    mag->scale_y  = MAG_CAL_DEFAULT_SY;
    mag->scale_z  = MAG_CAL_DEFAULT_SZ;
    mag->calibrated = true;
    printf("[Mag] factory cal off=(%.0f,%.0f,%.0f)\n",
           mag->offset_x, mag->offset_y, mag->offset_z);

    /* a session calibration in KV (tmpfs) overrides the factory one;
     * each key is applied independently */
    {
        int32_t v;
        bool overridden = false;
        if (kv_get_int("mag.ox", &v, 0) == 0) { mag->offset_x = v / 1000.0f; overridden = true; }
        if (kv_get_int("mag.oy", &v, 0) == 0) { mag->offset_y = v / 1000.0f; overridden = true; }
        if (kv_get_int("mag.oz", &v, 0) == 0) { mag->offset_z = v / 1000.0f; overridden = true; }
        if (kv_get_int("mag.sx", &v, 0) == 0) { mag->scale_x = v / 1000.0f; overridden = true; }
        if (kv_get_int("mag.sy", &v, 0) == 0) { mag->scale_y = v / 1000.0f; overridden = true; }
        if (kv_get_int("mag.sz", &v, 0) == 0) { mag->scale_z = v / 1000.0f; overridden = true; }
        if (overridden)
        {
            mag->calibrated = true;
            printf("[Mag] session cal override off=(%.0f,%.0f,%.0f)\n",
                   mag->offset_x, mag->offset_y, mag->offset_z);
        }
    }

    printf("[Mag] MMC5603NJ ready @0x%02x (pid=0x%02x)\n", addr, pid);
    return 0;
}

int mag_mmc5603_read(mag_mmc5603_t *mag)
{
    uint8_t raw[9];
    uint8_t st = 0;
    int32_t x, y, z;
    int tries;
    float m_norm;

    if (!mag->present || mag->fd < 0)
        return -1;

    /* One-shot trigger WITH auto SET/RESET: the chip demagnetises and
     * re-arms the AMR bridges right before every measurement, which
     * suppresses residual-magnetisation drift and temperature effects
     * (verified pattern used by Memsic-based drivers, e.g. bit 0x20). */
    i2c_write_reg(mag->fd, mag->i2c_addr, REG_CTRL0, CTRL0_TM_M | CTRL0_AUTO_SR);

    /* wait for STATUS bit6 (mag data ready) */
    for (tries = 0; tries < 150; tries++)
    {
        i2c_read_regs(mag->fd, mag->i2c_addr, REG_STATUS, &st, 1);
        if (st & STATUS_MAG_READY)
            break;
        usleep(2000);
    }
    if (!(st & STATUS_MAG_READY))
        return -1;

    /* self-test indicator (datasheet: bit5 "keeps low once the device
     * PASS self-test").  It is NOT a field-saturation flag and must not
     * gate the measurement - real saturation is checked by driving the
     * self-test coil via CTRL1 St_enp/St_enm.  Log it (rate limited) and
     * keep the sample. */
    if (st & STATUS_SAT_SENSOR)
    {
        static uint32_t last_st_log_ms;
        uint32_t tnow = get_time_ms();
        if (tnow - last_st_log_ms > 5000)
        {
            last_st_log_ms = tnow;
            printf("[Mag] STATUS1 self-test flag high (st=0x%02x)\n", st);
        }
    }

    /* 20-bit data, 9 bytes at 0x00 */
    if (i2c_read_regs(mag->fd, mag->i2c_addr, REG_OUT_X_L,
                      raw, sizeof(raw)) < 0)
        return -1;

    x = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (raw[6] >> 4);
    y = ((int32_t)raw[2] << 12) | ((int32_t)raw[3] << 4) | (raw[7] >> 4);
    z = ((int32_t)raw[4] << 12) | ((int32_t)raw[5] << 4) | (raw[8] >> 4);
    x -= (int32_t)1 << 19;
    y -= (int32_t)1 << 19;
    z -= (int32_t)1 << 19;

    /* counts -> uT (0.00625 uT/LSB); hard-iron offset, OTP comp and the
     * soft-iron scale from the figure-8 calibration all applied here */
    mag->x_g = ((float)x - mag->offset_x) * 0.00625f *
               mag->otp_comp[0] * mag->scale_x;
    mag->y_g = ((float)y - mag->offset_y) * 0.00625f *
               mag->otp_comp[1] * mag->scale_y;
    mag->z_g = ((float)z - mag->offset_z) * 0.00625f *
               mag->otp_comp[2] * mag->scale_z;

    /* PCB axis remap into the watch frame (X = 12 o'clock, Y = 3 o'clock,
     * Z = into the screen): the MMC5603NJ is mounted rotated 180 deg
     * about the Y axis relative to the IMU frame -- verified with the
     * turntable + cardinal-point experiments (2026-08-19) */
    {
        float tx = mag->x_g, tz = mag->z_g;
        mag->x_g = -tx;
        mag->z_g = -tz;
    }
    mag->x_raw = x;
    mag->y_raw = y;
    mag->z_raw = z;

    {
        m_norm = sqrtf(mag->x_g * mag->x_g +
                       mag->y_g * mag->y_g +
                       mag->z_g * mag->z_g);
        /* Earth's total field is 25..65 uT everywhere on the planet; with
         * a correct hard-iron calibration |B| is orientation-invariant,
         * so a wide window still catches gross calibration errors. */
        mag->healthy = (m_norm > 20.0f && m_norm < 80.0f);
    }

#if APP_DIAG_VERBOSE
    /* Raw-frame diagnostic: STATUS1 byte (bit5 = Sat_sensor self-test flag,
     * bit6 = Meas_m_done, bit4 = OTP_read_done), raw 20-bit counts and the
     * calibrated uT vector - lets the gating/calibration be judged from a
     * plain serial log. */
    {
        static uint32_t last_ms;
        uint32_t t = get_time_ms();
        if (t - last_ms >= 500)
        {
            last_ms = t;
            printf("[MagS] st=0x%02x raw=(%ld,%ld,%ld) uT=(%.1f,%.1f,%.1f) "
                   "|B|=%.1f ok=%d\n",
                   st, (long)x, (long)y, (long)z,
                   mag->x_g, mag->y_g, mag->z_g, m_norm,
                   (int)mag->healthy);
        }
    }
#endif

    return 0;
}

void mag_mmc5603_start_calib(mag_mmc5603_t *mag, uint32_t duration_ms)
{
    mag->calib_running = true;
    mag->calib_end_ms = get_time_ms() + duration_ms;
    mag->calib_min_x = mag->calib_min_y = mag->calib_min_z = 0x7FFFFFFF;
    mag->calib_max_x = mag->calib_max_y = mag->calib_max_z = -0x7FFFFFFF;
    memset(mag->fit_ata, 0, sizeof(mag->fit_ata));
    memset(mag->fit_atb, 0, sizeof(mag->fit_atb));
    mag->fit_n = 0;
    printf("[Mag] Calibrating for %ums (tumble the watch in 3D)...\n",
           (unsigned)duration_ms);
}

/* Solve the 4x4 normal equations in place (Gauss-Jordan with partial
 * pivoting).  Returns false if the system is singular (poor coverage). */
static bool solve4(double m[4][5], double out[4])
{
    int i, j, k;

    for (i = 0; i < 4; i++)
    {
        int piv = i;
        for (k = i + 1; k < 4; k++)
            if (fabs(m[k][i]) > fabs(m[piv][i]))
                piv = k;
        if (fabs(m[piv][i]) < 1e-9)
            return false;
        if (piv != i)
            for (j = 0; j < 5; j++)
            {
                double t = m[i][j];
                m[i][j] = m[piv][j];
                m[piv][j] = t;
            }
        for (k = 0; k < 4; k++)
        {
            double f;
            if (k == i)
                continue;
            f = m[k][i] / m[i][i];
            for (j = i; j < 5; j++)
                m[k][j] -= f * m[i][j];
        }
    }
    for (i = 0; i < 4; i++)
        out[i] = m[i][4] / m[i][i];
    return true;
}

void mag_mmc5603_calib_step(mag_mmc5603_t *mag)
{
    float avg_range;

    if (!mag->calib_running)
        return;

    if (mag->calib_end_ms != 0 && get_time_ms() >= mag->calib_end_ms)
    {
        float rx = (float)(mag->calib_max_x - mag->calib_min_x);
        float ry = (float)(mag->calib_max_y - mag->calib_min_y);
        float rz = (float)(mag->calib_max_z - mag->calib_min_z);
        double a[4][5];
        double sol[4];
        float cx, cy, cz, radius_uT;
        int i;

        /* Coverage check: a real 3D calibration must sweep X, Y and Z by
         * at least ~2000 counts (~12 uT) each.  A flat figure-8 leaves Z
         * un-illuminated, so the fitted centre would absorb the Earth's
         * vertical field and corrupt the tilt-compensated heading. */
        if (rx < 2000.0f || ry < 2000.0f || rz < 2000.0f || mag->fit_n < 80)
        {
            printf("[Mag] Calibration rejected (x %.0f, y %.0f, z %.0f, n %u); "
                   "rotate the watch in 3D (tumble it) and retry, "
                   "keeping previous cal\n",
                   rx, ry, rz, (unsigned)mag->fit_n);
            mag->calib_running = false;
            return;
        }

        for (i = 0; i < 4; i++)
        {
            int j;
            for (j = 0; j < 4; j++)
                a[i][j] = mag->fit_ata[i][j];
            a[i][4] = mag->fit_atb[i];
        }
        if (!solve4(a, sol))
        {
            printf("[Mag] Calibration fit singular; keeping previous cal\n");
            mag->calib_running = false;
            return;
        }

        cx = (float)sol[0];
        cy = (float)sol[1];
        cz = (float)sol[2];
        radius_uT = (float)sqrt(sol[3] + sol[0] * sol[0] +
                                sol[1] * sol[1] + sol[2] * sol[2]) *
                    0.00625f;

        /* Physical self-check: the fitted sphere radius IS the local
         * geomagnetic total field, which is 25..65 uT anywhere on Earth.
         * Anything else means the tumble was near a magnet/motor, was not
         * a real rotation, or the data was corrupted. */
        if (radius_uT < 25.0f || radius_uT > 65.0f)
        {
            printf("[Mag] Calibration rejected: fitted |B| = %.1f uT "
                   "(expected 25..65 uT); keep away from metal and retry\n",
                   radius_uT);
            mag->calib_running = false;
            return;
        }

        /* Fit-quality gate: the RMS distance of the samples from the
         * fitted centre must match the fitted radius.  A tumble that
         * passed near metal biases the centre and shows up here (e.g. a
         * 9 uT-biased fit fitted 41 uT where the true field is 47 uT). */
        {
            double n = (double)mag->fit_n;
            double mx = mag->fit_ata[0][3] / 2.0 / n;   /* mean x */
            double my = mag->fit_ata[1][3] / 2.0 / n;
            double mz = mag->fit_ata[2][3] / 2.0 / n;
            double mean_sq = mag->fit_atb[3] / n;       /* mean |v|^2 */
            double rms_sq = mean_sq - 2.0 * (sol[0] * mx + sol[1] * my +
                                             sol[2] * mz) +
                            (sol[0] * sol[0] + sol[1] * sol[1] +
                             sol[2] * sol[2]);
            double r_fit_sq = sol[3] + sol[0] * sol[0] + sol[1] * sol[1] +
                              sol[2] * sol[2];
            double rms_uT = sqrt(rms_sq) * 0.00625;
            double dev = (rms_uT - radius_uT) / radius_uT;

            if (dev < 0.0)
                dev = -dev;
            if (dev > 0.08)
            {
                printf("[Mag] Calibration rejected: RMS radius %.1f uT vs "
                       "fitted %.1f uT (%.0f%% deviation) - tumble away "
                       "from metal/desk and retry\n",
                       rms_uT, radius_uT, dev * 100.0);
                mag->calib_running = false;
                return;
            }
            printf("[Mag] fit quality: RMS %.1f uT vs radius %.1f uT "
                   "(%.1f%% dev)\n", rms_uT, radius_uT, dev * 100.0);
        }

        mag->offset_x = cx;
        mag->offset_y = cy;
        mag->offset_z = cz;

        avg_range = (rx + ry + rz) / 3.0f;
        mag->scale_x = rx > 0.0f ? avg_range / rx : 1.0f;
        mag->scale_y = ry > 0.0f ? avg_range / ry : 1.0f;
        mag->scale_z = rz > 0.0f ? avg_range / rz : 1.0f;
        mag->calibrated = true;
        mag->calib_running = false;
        printf("[Mag] Calibrated (LSQ sphere) off=(%.0f,%.0f,%.0f) "
               "scale=(%.3f,%.3f,%.3f) |B|=%.1f uT n=%u\n",
               mag->offset_x, mag->offset_y, mag->offset_z,
               mag->scale_x, mag->scale_y, mag->scale_z,
               radius_uT, (unsigned)mag->fit_n);
        /* session persistence (tmpfs only): the compiled-in defaults are
         * the only values that survive a reboot, so the KV copy exists
         * just to carry a fresh calibration to the running boot */
        kv_set_int("mag.ox", (int32_t)(mag->offset_x * 1000.0f));
        kv_set_int("mag.oy", (int32_t)(mag->offset_y * 1000.0f));
        kv_set_int("mag.oz", (int32_t)(mag->offset_z * 1000.0f));
        kv_set_int("mag.sx", (int32_t)(mag->scale_x * 1000.0f));
        kv_set_int("mag.sy", (int32_t)(mag->scale_y * 1000.0f));
        kv_set_int("mag.sz", (int32_t)(mag->scale_z * 1000.0f));
        return;
    }

    if (mag_mmc5603_read(mag) == 0)
    {
        double x = (double)mag->x_raw;
        double y = (double)mag->y_raw;
        double z = (double)mag->z_raw;
        double row[4];
        double t = x * x + y * y + z * z;
        int i, j;

        if (mag->x_raw < mag->calib_min_x) mag->calib_min_x = mag->x_raw;
        if (mag->x_raw > mag->calib_max_x) mag->calib_max_x = mag->x_raw;
        if (mag->y_raw < mag->calib_min_y) mag->calib_min_y = mag->y_raw;
        if (mag->y_raw > mag->calib_max_y) mag->calib_max_y = mag->y_raw;
        if (mag->z_raw < mag->calib_min_z) mag->calib_min_z = mag->z_raw;
        if (mag->z_raw > mag->calib_max_z) mag->calib_max_z = mag->z_raw;

        /* accumulate the normal equations for x^2+y^2+z^2 = 2a x + 2b y
         * + 2c z + d  (centre = (a,b,c), radius^2 = d + a^2+b^2+c^2) */
        row[0] = 2.0 * x;
        row[1] = 2.0 * y;
        row[2] = 2.0 * z;
        row[3] = 1.0;
        for (i = 0; i < 4; i++)
        {
            for (j = 0; j < 4; j++)
                mag->fit_ata[i][j] += row[i] * row[j];
            mag->fit_atb[i] += row[i] * t;
        }
        mag->fit_n++;
    }
}

void mag_mmc5603_calibrate(mag_mmc5603_t *mag, uint32_t duration_ms)
{
    mag_mmc5603_start_calib(mag, duration_ms);
    while (mag->calib_running)
    {
        mag_mmc5603_calib_step(mag);
        usleep(15000);
    }
}

void mag_mmc5603_deinit(mag_mmc5603_t *mag)
{
    if (mag->fd >= 0)
    {
        close(mag->fd);
        mag->fd = -1;
    }
    mag->present = false;
}
