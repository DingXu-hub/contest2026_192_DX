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

#define STATUS_MAG_READY  0x40   /* bit6: measurement data ready */
#define STATUS_TEMP_READY 0x80   /* bit7 */
#define STATUS_SATURATED  0x20   /* bit5: sensor saturated (strong field) */
#define STATUS_OTP_DONE   0x10   /* bit4: OTP load done */

/* historical aliases used at init (SET/RESET pulses) */
#define CTRL0_SET    CTRL0_DO_SET
#define CTRL0_RESET  CTRL0_DO_RESET

/* Compile-time (factory) calibration in raw ADC counts.  The board has
 * NO persistent filesystem: /data is tmpfs (lost on reboot) and runtime
 * NOR write/erase hard-faults the SiFli HAL on this XIP build, so the
 * calibration that survives reboots is baked into the binary here.
 *
 * SINGLE SOURCE OF TRUTH: this is the only definition of
 * MAG_CAL_DEFAULT_* (the stale copies in mag_mmc5603.h were removed so a
 * hidden macro redefinition cannot shadow these).  Values below come from
 * the 2026-08-22 on-device figure-8 run; update them after a fresh
 * calibration and reflash.  After a good 3D tumble also backfill the
 * resulting scale factors here (they are currently 1.0, i.e. per-axis
 * sensitivity differences are uncorrected until then). */
#define MAG_CAL_DEFAULT_OX (-3048.0f)
#define MAG_CAL_DEFAULT_OY 3101.0f
#define MAG_CAL_DEFAULT_OZ (-9498.0f)
#define MAG_CAL_DEFAULT_SX 1.0f
#define MAG_CAL_DEFAULT_SY 1.0f
#define MAG_CAL_DEFAULT_SZ 1.0f

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

    /* Adafruit sequence: SW_RST -> SET/RESET -> one-shot mode */
    i2c_write_reg(mag->fd, addr, REG_CTRL1, 0x80);   /* SW_RST (auto-clear) */
    usleep(20000);
    i2c_write_reg(mag->fd, addr, REG_CTRL0, CTRL0_SET);    /* SET pulse */
    usleep(2000);
    i2c_write_reg(mag->fd, addr, REG_CTRL0, CTRL0_RESET);  /* RESET pulse */
    usleep(2000);
    i2c_write_reg(mag->fd, addr, REG_CTRL0, CTRL0_CMM_FREQ); /* one-shot */
    i2c_write_reg(mag->fd, addr, REG_CTRL2, 0x00);   /* no CMM */

    mag->scale_x = mag->scale_y = mag->scale_z = 1.0f;
    mag->otp_comp[0] = mag->otp_comp[1] = mag->otp_comp[2] = 1.0f;

    /* compile-time default calibration (raw counts, from an on-device
     * figure-8 run): the board has no persistent FS (data is tmpfs and
     * runtime NOR writes hard-fault on this XIP build), so the
     * calibration that matters is baked in here.  A session KV copy can
     * still override it until the next reboot.
     *   2026-08-19 runs: off=(-2443, 2529, -11087/…), scale=(1,1,1)
     *   X/Y are reliable (flat-figure-8).  The Z offset used to absorb
     *   the Earth's vertical field (mz ~= 0 at level) which corrupted the
     *   tilt-compensated heading at tilt (measured -44 deg error at 41
     *   deg tilt).  Re-derived from the level raw Z and the local field
     *   (~41 uT vertical): OZ = -4500 -> mz ~= +41 uT into the screen. */
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
    static uint32_t last_sat_log_ms;

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

    /* saturation flag (bit5): the sensor is being overpowered by a strong
     * field (hand magnet / motor).  The reading is unreliable - drop the
     * sample instead of feeding a wrong direction into the fusion. */
    if (st & STATUS_SATURATED)
    {
        mag->healthy = false;
        if (get_time_ms() - last_sat_log_ms > 5000)
        {
            last_sat_log_ms = get_time_ms();
            printf("[Mag] saturated sample dropped\n");
        }
        return -1;
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
        mag->healthy = (m_norm > 10.0f && m_norm < 200.0f);
    }

    return 0;
}

void mag_mmc5603_start_calib(mag_mmc5603_t *mag, uint32_t duration_ms)
{
    mag->calib_running = true;
    mag->calib_end_ms = get_time_ms() + duration_ms;
    mag->calib_min_x = mag->calib_min_y = mag->calib_min_z = 0x7FFFFFFF;
    mag->calib_max_x = mag->calib_max_y = mag->calib_max_z = -0x7FFFFFFF;
    printf("[Mag] Calibrating for %ums (rotate the watch)...\n",
           (unsigned)duration_ms);
}

void mag_mmc5603_calib_step(mag_mmc5603_t *mag)
{
    float avg_range;

    if (!mag->calib_running)
        return;

    if (get_time_ms() >= mag->calib_end_ms)
    {
        float rx = (float)(mag->calib_max_x - mag->calib_min_x);
        float ry = (float)(mag->calib_max_y - mag->calib_min_y);
        float rz = (float)(mag->calib_max_z - mag->calib_min_z);

        /* Coverage check: a real 3D calibration must sweep X, Y and Z by
         * at least ~2000 counts (~12 uT) each.  A flat-only figure-8
         * leaves Z un-illuminated, producing a garbage Z offset (it
         * absorbs the Earth's vertical field, which corrupts the
         * tilt-compensated heading at tilt) and a Z divide-by-zero
         * (=> NaN => permanently "unhealthy"); reject the run and keep
         * the previous calibration.  Tell the user to tumble the watch
         * through ALL orientations (draw big circles in the air). */
        if (rx < 2000.0f || ry < 2000.0f || rz < 2000.0f)
        {
            printf("[Mag] Calibration rejected (x %.0f, y %.0f, z %.0f); "
                   "rotate the watch in 3D (tumble it) and retry, "
                   "keeping previous cal\n", rx, ry, rz);
            mag->calib_running = false;
            return;
        }

        mag->offset_x = (float)(mag->calib_min_x + mag->calib_max_x) / 2.0f;
        mag->offset_y = (float)(mag->calib_min_y + mag->calib_max_y) / 2.0f;
        mag->offset_z = (float)(mag->calib_min_z + mag->calib_max_z) / 2.0f;

        avg_range = (rx + ry + rz) / 3.0f;
        mag->scale_x = rx > 0.0f ? avg_range / rx : 1.0f;
        mag->scale_y = ry > 0.0f ? avg_range / ry : 1.0f;
        mag->scale_z = rz > 0.0f ? avg_range / rz : 1.0f;
        mag->calibrated = true;
        mag->calib_running = false;
        printf("[Mag] Calibrated off=(%.0f,%.0f,%.0f) scale=(%.2f,%.2f,%.2f)\n",
               mag->offset_x, mag->offset_y, mag->offset_z,
               mag->scale_x, mag->scale_y, mag->scale_z);
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
        if (mag->x_raw < mag->calib_min_x) mag->calib_min_x = mag->x_raw;
        if (mag->x_raw > mag->calib_max_x) mag->calib_max_x = mag->x_raw;
        if (mag->y_raw < mag->calib_min_y) mag->calib_min_y = mag->y_raw;
        if (mag->y_raw > mag->calib_max_y) mag->calib_max_y = mag->y_raw;
        if (mag->z_raw < mag->calib_min_z) mag->calib_min_z = mag->z_raw;
        if (mag->z_raw > mag->calib_max_z) mag->calib_max_z = mag->z_raw;
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
