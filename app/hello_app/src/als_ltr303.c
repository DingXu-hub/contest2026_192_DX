/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * als_ltr303.c - LTR-303ALS-01 ambient light sensor, raw I2C.
 *
 * Register map (LTR303ALS-01 datasheet, register bit 7 set = auto
 * increment on multi-byte reads):
 *   0x80  ALS_CONTR    (bit0 active, bit1 SW_RESET, bits[5:2] gain)
 *   0x81  ALS_MEAS_RATE(IT in bits[6:3], rate in bits[2:0])
 *   0x84/0x85  CH1 (IR) low/high
 *   0x86/0x87  CH0 (vis+IR) low/high
 *   0x8A  STATUS      (bit2 = new data)
 *
 * Lux is computed with the datasheet piecewise formula for
 * integration time 100 ms and gain 1x.
 */

#include <nuttx/config.h>
#include <nuttx/i2c/i2c_master.h>

#include "als_ltr303.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define LTR303_REG_CONTR     0x80
#define LTR303_REG_MEAS_RATE 0x81
#define LTR303_REG_CH1_LOW   0x84
#define LTR303_REG_CH0_LOW   0x86
#define LTR303_REG_STATUS    0x8A

#define LTR303_CONTR_ACTIVE  0x01
#define LTR303_CONTR_GAIN_1X 0x00
#define LTR303_IT_100MS_RATE_200MS  0x02

static int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val)
{
    struct i2c_msg_s msg;
    struct i2c_transfer_s xfer;
    uint8_t buf[2];

    buf[0] = reg;
    buf[1] = val;
    msg.frequency = 400000;
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

    msgs[0].frequency = 400000;
    msgs[0].addr = addr;
    msgs[0].flags = 0;
    msgs[0].buffer = &reg;
    msgs[0].length = 1;

    msgs[1].frequency = 400000;
    msgs[1].addr = addr;
    msgs[1].flags = I2C_M_READ;
    msgs[1].buffer = buf;
    msgs[1].length = len;

    xfer.msgv = msgs;
    xfer.msgc = 2;

    return ioctl(fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
}

static uint16_t ltr303_lux(uint16_t ch0, uint16_t ch1)
{
    float lux;
    float ratio;

    if (ch0 == 0)
        return 0;

    ratio = (float)ch1 / (float)ch0;

    /* LTR303 datasheet, IT=100 ms, gain=1x */
    if (ratio <= 0.5f)
        lux = (float)ch0 * 0.0304f - (float)ch1 * 0.062f;
    else if (ratio <= 0.61f)
        lux = (float)ch0 * 0.0224f - (float)ch1 * 0.031f;
    else if (ratio <= 0.80f)
        lux = (float)ch0 * 0.0128f - (float)ch1 * 0.0153f;
    else if (ratio <= 1.30f)
        lux = (float)ch0 * 0.00146f - (float)ch1 * 0.00112f;
    else
        lux = 0.0f;

    if (lux < 0.0f)
        lux = 0.0f;
    if (lux > 65535.0f)
        lux = 65535.0f;

    return (uint16_t)lux;
}

int als_ltr303_init(als_ltr303_t *als, const char *i2c_dev)
{
    uint8_t buf[4];

    memset(als, 0, sizeof(*als));
    als->fd = -1;

    als->fd = open(i2c_dev, O_RDWR);
    if (als->fd < 0)
    {
        printf("[ALS] open %s failed\n", i2c_dev);
        return -1;
    }

    /* probe: read the two data registers; the device ACKs even in standby */
    if (i2c_read_regs(als->fd, LTR303_I2C_ADDR, LTR303_REG_CH0_LOW,
                      buf, sizeof(buf)) < 0)
    {
        printf("[ALS] LTR303 not present\n");
        close(als->fd);
        als->fd = -1;
        return -1;
    }

    /* standby -> active, gain 1x, IT=100ms, rate=200ms */
    i2c_write_reg(als->fd, LTR303_I2C_ADDR, LTR303_REG_CONTR,
                  LTR303_CONTR_ACTIVE | LTR303_CONTR_GAIN_1X);
    i2c_write_reg(als->fd, LTR303_I2C_ADDR, LTR303_REG_MEAS_RATE,
                  LTR303_IT_100MS_RATE_200MS);

    als->present = true;
    printf("[ALS] LTR303ALS ready\n");
    return 0;
}

int als_ltr303_read(als_ltr303_t *als)
{
    uint8_t buf[4];
    uint16_t ch0, ch1;

    if (!als->present || als->fd < 0)
        return -1;

    if (i2c_read_regs(als->fd, LTR303_I2C_ADDR, LTR303_REG_CH1_LOW,
                      buf, sizeof(buf)) < 0)
        return -1;

    ch1 = (uint16_t)(buf[0] | (buf[1] << 8));
    ch0 = (uint16_t)(buf[2] | (buf[3] << 8));

    als->ch0 = ch0;
    als->ch1 = ch1;
    als->lux = ltr303_lux(ch0, ch1);

    /* 1-pole low pass (~2 s time constant at 0.5 Hz sampling) */
    als->filtered_lux = (uint16_t)(0.6f * als->filtered_lux + 0.4f * als->lux);

    return 0;
}

void als_ltr303_deinit(als_ltr303_t *als)
{
    if (als->fd >= 0)
    {
        /* back to standby to save power */
        i2c_write_reg(als->fd, LTR303_I2C_ADDR, LTR303_REG_CONTR, 0x00);
        close(als->fd);
        als->fd = -1;
    }
    als->present = false;
}
