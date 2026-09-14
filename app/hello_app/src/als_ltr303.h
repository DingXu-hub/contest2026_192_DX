/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * als_ltr303.h - LTR-303ALS-01 ambient light sensor over the NuttX I2C
 * char driver (/dev/i2c1).
 */

#ifndef __ALS_LTR303_H
#define __ALS_LTR303_H

#include <stdint.h>
#include <stdbool.h>

#define LTR303_I2C_ADDR   0x29

typedef struct {
    int      fd;
    bool     present;
    uint16_t ch0;            /* visible+IR channel */
    uint16_t ch1;            /* IR channel */
    uint16_t lux;            /* computed illuminance */
    uint16_t filtered_lux;
} als_ltr303_t;

int  als_ltr303_init(als_ltr303_t *als, const char *i2c_dev);
int  als_ltr303_read(als_ltr303_t *als);
void als_ltr303_deinit(als_ltr303_t *als);

#endif /* __ALS_LTR303_H */
